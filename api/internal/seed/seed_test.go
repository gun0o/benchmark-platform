package seed_test

import (
	"encoding/json"
	"math"
	"testing"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/seed"
)

func opts() seed.Options {
	o := seed.Defaults()
	o.Machines = 2
	o.Trials = 20
	o.Now = time.Date(2026, 9, 21, 12, 0, 0, 0, time.UTC) // fixed, so the test is stable
	return o
}

// The milestone's own verification: generate twice with one seed and diff the output.
func TestDeterministicUnderTheSameSeed(t *testing.T) {
	a, err := json.Marshal(seed.Generate(opts()))
	if err != nil {
		t.Fatal(err)
	}
	b, err := json.Marshal(seed.Generate(opts()))
	if err != nil {
		t.Fatal(err)
	}
	if string(a) != string(b) {
		t.Fatal("two generations with the same seed differ")
	}

	other := opts()
	other.Seed = 43
	c, err := json.Marshal(seed.Generate(other))
	if err != nil {
		t.Fatal(err)
	}
	if string(a) == string(c) {
		t.Fatal("a different seed produced the same document")
	}
}

// Everything the seeder emits must pass the same validation as engine output: it goes
// through the real ingest path, so anything else would be a fixture that only works here.
func TestEveryEnvelopeValidates(t *testing.T) {
	envs := seed.Generate(opts())
	if len(envs) == 0 {
		t.Fatal("no envelopes")
	}
	seenRunIDs := map[string]bool{}
	total := 0
	for _, env := range envs {
		if err := env.Validate(); err != nil {
			t.Fatalf("run %s: %v", env.RunID, err)
		}
		if seenRunIDs[env.RunID] {
			t.Fatalf("run id %s generated twice", env.RunID)
		}
		seenRunIDs[env.RunID] = true
		if env.Machine.Hostname[:9] != "synthetic" || env.Machine.EngineVersion != "seed" {
			t.Fatalf("synthetic machine is not labelled as one: %+v", env.Machine)
		}
		total += len(env.Results)
	}
	if total != opts().Rows() {
		t.Fatalf("generated %d rows, Rows() predicted %d", total, opts().Rows())
	}
}

func TestDefaultsClearTheHundredThousandRowFloor(t *testing.T) {
	if rows := seed.Defaults().Rows(); rows < 100_000 {
		t.Fatalf("the default options generate %d rows, below the 100,000 floor", rows)
	}
}

func TestAllTwelveMetricsAppearForEveryMachine(t *testing.T) {
	envs := seed.Generate(opts())
	perMachine := map[string]map[string]int{}
	for _, env := range envs {
		for i := range env.Results {
			r := &env.Results[i]
			if perMachine[env.Machine.ID] == nil {
				perMachine[env.Machine.ID] = map[string]int{}
			}
			perMachine[env.Machine.ID][r.Metric]++
		}
	}
	if len(perMachine) != 2 {
		t.Fatalf("%d machines, want 2", len(perMachine))
	}
	for id, metrics := range perMachine {
		if len(metrics) != len(model.Metrics) {
			t.Fatalf("machine %s has %d metrics, want %d", id, len(metrics), len(model.Metrics))
		}
		for _, m := range model.Metrics {
			if metrics[m] == 0 {
				t.Fatalf("machine %s has no %s", id, m)
			}
		}
	}
}

// Timestamps have to spread over the 30-day window, or every trend chart is a vertical
// line.
func TestTimestampsSpreadOverThirtyDays(t *testing.T) {
	envs := seed.Generate(opts())
	var first, last time.Time
	for _, env := range envs {
		ts, err := model.ParseTimestamp(env.StartedAt)
		if err != nil {
			t.Fatal(err)
		}
		if first.IsZero() || ts.Before(first) {
			first = ts
		}
		if ts.After(last) {
			last = ts
		}
	}
	span := last.Sub(first)
	if span < 10*24*time.Hour || span > 30*24*time.Hour {
		t.Fatalf("runs span %v, want most of a 30-day window", span)
	}
}

// --- the models --------------------------------------------------------------------

// meanOf is the sample mean of a configuration's values, for checking the shape of a curve
// without being fooled by one noisy trial.
func meanOf(t *testing.T, envs []*model.RunEnvelope, machine, metric string, threads int, ws int64) float64 {
	t.Helper()
	sum, n := 0.0, 0
	for _, env := range envs {
		if env.Machine.Hostname != machine {
			continue
		}
		for i := range env.Results {
			r := &env.Results[i]
			if r.Metric == metric && r.ThreadCount == threads && r.WorkingSetBytes == ws {
				sum += r.Value
				n++
			}
		}
	}
	if n == 0 {
		t.Fatalf("no %s rows for %s at %d threads, ws %d", metric, machine, threads, ws)
	}
	return sum / float64(n)
}

func TestCPUScalingBendsButNeverFalls(t *testing.T) {
	envs := seed.Generate(opts())
	prev := 0.0
	prevRatio := math.Inf(1)
	for _, n := range seed.ThreadCounts {
		v := meanOf(t, envs, "synthetic-01", "cpu_int_ops", n, 0)
		if v <= prev {
			t.Fatalf("throughput fell from %d threads: %.3g -> %.3g", n/2, prev, v)
		}
		if prev > 0 {
			ratio := v / prev
			if ratio > 2.0001 {
				t.Fatalf("scaling above linear at %d threads: %.3fx", n, ratio)
			}
			if ratio > prevRatio {
				t.Fatalf("scaling improved with more threads at %d: %.3f then %.3f",
					n, prevRatio, ratio)
			}
			prevRatio = ratio
		}
		prev = v
	}
	// The small cloud instance must scale visibly worse than the server part.
	server := meanOf(t, envs, "synthetic-01", "cpu_int_ops", 16, 0) /
		meanOf(t, envs, "synthetic-01", "cpu_int_ops", 1, 0)
	laptop := meanOf(t, envs, "synthetic-02", "cpu_int_ops", 16, 0) /
		meanOf(t, envs, "synthetic-02", "cpu_int_ops", 1, 0)
	if server <= laptop {
		t.Fatalf("64-core server scales %.1fx at 16 threads, 14-core laptop %.1fx", server, laptop)
	}
}

func TestLatencyClimbsWithTheWorkingSetAndStepsAtTheCaches(t *testing.T) {
	envs := seed.Generate(opts())
	prev := 0.0
	for _, ws := range []int64{4 << 10, 32 << 10, 128 << 10, 512 << 10, 2 << 20, 8 << 20,
		32 << 20, 256 << 20, 1 << 30} {
		v := meanOf(t, envs, "synthetic-01", "mem_latency", 1, ws)
		if v < prev {
			t.Fatalf("latency fell at %d bytes: %.2f -> %.2f ns", ws, prev, v)
		}
		prev = v
	}
	l1 := meanOf(t, envs, "synthetic-01", "mem_latency", 1, 4<<10)
	dram := meanOf(t, envs, "synthetic-01", "mem_latency", 1, 1<<30)
	if dram/l1 < 20 {
		t.Fatalf("DRAM is only %.1fx L1; the hierarchy has no shape", dram/l1)
	}
}

func TestBandwidthPlateausInsteadOfScalingForever(t *testing.T) {
	envs := seed.Generate(opts())
	// At a DRAM-sized working set the aggregate must stop growing well before 16x.
	one := meanOf(t, envs, "synthetic-02", "mem_read_bw", 1, 512<<20)
	sixteen := meanOf(t, envs, "synthetic-02", "mem_read_bw", 16, 512<<20)
	if sixteen <= one {
		t.Fatalf("bandwidth did not grow with threads: %.1f -> %.1f GB/s", one, sixteen)
	}
	if sixteen/one > 8 {
		t.Fatalf("DRAM bandwidth scaled %.1fx with 16 threads; it should saturate", sixteen/one)
	}
	// And a write must cost more than a read once the set leaves L1 (write-allocate).
	read := meanOf(t, envs, "synthetic-02", "mem_read_bw", 4, 64<<20)
	write := meanOf(t, envs, "synthetic-02", "mem_write_bw", 4, 64<<20)
	if write >= read {
		t.Fatalf("write %.1f GB/s is not below read %.1f GB/s at a DRAM working set", write, read)
	}
}

func TestDiskRandomIOPSSaturatesWithQueueDepth(t *testing.T) {
	envs := seed.Generate(opts())
	prev, prevGain := 0.0, math.Inf(1)
	for _, qd := range seed.ThreadCounts {
		v := meanOf(t, envs, "synthetic-01", "disk_rand_read_iops", qd, 1<<30)
		if v <= prev {
			t.Fatalf("IOPS fell at queue depth %d", qd)
		}
		if prev > 0 {
			gain := v / prev
			if gain > prevGain {
				t.Fatalf("IOPS gain grew with queue depth at %d: %.2f then %.2f", qd, prevGain, gain)
			}
			prevGain = gain
		}
		prev = v
	}
}

// The per-trial noise has to come out at the sigma the profile asked for, because M5.2's
// verification compares the CoV computed in SQL against these numbers.
func TestNoiseMatchesTheProfileSigma(t *testing.T) {
	o := opts()
	o.Trials = 400
	envs := seed.Generate(o)
	for _, tc := range []struct {
		machine string
		sigma   float64
	}{{"synthetic-01", 0.010}, {"synthetic-02", 0.025}} {
		var vals []float64
		for _, env := range envs {
			if env.Machine.Hostname != tc.machine {
				continue
			}
			for i := range env.Results {
				r := &env.Results[i]
				if r.Metric == "cpu_int_ops" && r.ThreadCount == 4 {
					vals = append(vals, r.Value)
				}
			}
		}
		if len(vals) < 100 {
			t.Fatalf("%s: only %d values", tc.machine, len(vals))
		}
		mean, m2 := 0.0, 0.0
		for i, v := range vals { // Welford
			d := v - mean
			mean += d / float64(i+1)
			m2 += d * (v - mean)
		}
		cov := math.Sqrt(m2/float64(len(vals)-1)) / mean
		if math.Abs(cov-tc.sigma)/tc.sigma > 0.20 {
			t.Fatalf("%s: CoV %.4f, profile sigma %.4f (more than 20%% apart)",
				tc.machine, cov, tc.sigma)
		}
		t.Logf("%s cpu_int_ops @4 threads: CoV %.3f%% against sigma %.3f%% (%d trials)",
			tc.machine, cov*100, tc.sigma*100, len(vals))
	}
}

func TestMachineIDsFollowTheEnginesRule(t *testing.T) {
	// Same fields, same id; one field different, different id.
	ps := seed.Profiles()
	if seed.MachineID(ps[0]) != seed.MachineID(ps[0]) {
		t.Fatal("machine id is not a function of the profile")
	}
	changed := ps[0]
	changed.Hostname = "synthetic-01x"
	if seed.MachineID(changed) == seed.MachineID(ps[0]) {
		t.Fatal("hostname is part of the id but did not change it")
	}
	if len(seed.MachineID(ps[0])) != 32 {
		t.Fatalf("machine id is %d chars, want 32", len(seed.MachineID(ps[0])))
	}
}
