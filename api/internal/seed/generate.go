package seed

import (
	"encoding/json"
	"fmt"
	"math"
	"math/rand/v2"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

// Options control a generation.
type Options struct {
	Machines       int // how many of Profiles() to use
	Trials         int // trials per configuration
	Seed           uint64
	RunsPerMachine int       // configurations are split across this many runs, on different days
	Now            time.Time // the end of the 30-day window; zero means time.Now()
}

// Defaults are what `go run ./cmd/seed` uses with no flags: 5 machines x 100 trials, which
// is over the 100,000-row floor the milestone requires.
func Defaults() Options {
	return Options{Machines: 5, Trials: 100, Seed: 42, RunsPerMachine: 6}
}

// config is one (metric, thread_count, working_set) combination of one machine.
type config struct {
	workload   string
	metric     string
	unit       string
	threads    int
	workingSet int64
}

// ThreadCounts is the sweep every metric is generated over. Real thread counts above a
// machine's logical CPU count still happen (oversubscription is a thing people measure),
// so they are not filtered out; the model makes them slower, which is the honest answer.
var ThreadCounts = []int{1, 2, 4, 8, 16}

// The working-set sweeps. Bandwidth walks the hierarchy; latency walks it more finely,
// because that is the curve with the interesting steps; disk working sets are file sizes.
var (
	bandwidthSets = []int64{32 << 10, 256 << 10, 2 << 20, 8 << 20, 64 << 20, 512 << 20}
	latencySets   = []int64{4 << 10, 32 << 10, 128 << 10, 512 << 10, 2 << 20, 8 << 20, 32 << 20, 256 << 20, 1 << 30}
	diskSets      = []int64{1 << 30, 2 << 30, 4 << 30}
)

// configs enumerates every configuration of one machine, in a fixed order so that a given
// seed always produces the same document.
func configs() []config {
	var out []config
	for _, m := range []struct{ workload, metric string }{
		{"cpu_int", "cpu_int_ops"}, {"cpu_fp", "cpu_fp_ops"}, {"cpu_hash", "cpu_hash_ops"},
	} {
		for _, t := range ThreadCounts {
			out = append(out, config{m.workload, m.metric, "ops/s", t, 0})
		}
	}
	for _, metric := range []string{"mem_read_bw", "mem_write_bw", "mem_copy_bw"} {
		for _, ws := range bandwidthSets {
			for _, t := range ThreadCounts {
				out = append(out, config{"mem_bw", metric, "GB/s", t, ws})
			}
		}
	}
	for _, ws := range latencySets {
		for _, t := range ThreadCounts {
			out = append(out, config{"mem_latency", "mem_latency", "ns", t, ws})
		}
	}
	for _, m := range []struct{ workload, metric, unit string }{
		{"disk_seq", "disk_seq_read_bw", "MB/s"},
		{"disk_seq", "disk_seq_write_bw", "MB/s"},
		{"disk_rand", "disk_rand_read_iops", "IOPS"},
		{"disk_rand", "disk_rand_write_iops", "IOPS"},
		{"disk_rand", "disk_rand_read_p99_us", "us"},
	} {
		for _, ws := range diskSets {
			for _, t := range ThreadCounts {
				out = append(out, config{m.workload, m.metric, m.unit, t, ws})
			}
		}
	}
	return out
}

// ConfigCount is how many configurations each machine has; Rows is the total the options
// will produce. Both are exact, so the CLI can print the count before generating.
func ConfigCount() int { return len(configs()) }

// Rows is the exact number of measurement rows these options generate.
func (o Options) Rows() int { return o.Machines * ConfigCount() * o.Trials }

// base is the model's expected value for one configuration, before per-trial noise.
func (p Profile) base(c config) float64 {
	switch c.metric {
	case "cpu_int_ops":
		return p.CPUThroughput(p.IntOps1, c.threads)
	case "cpu_fp_ops":
		return p.CPUThroughput(p.FPOps1, c.threads)
	case "cpu_hash_ops":
		return p.CPUThroughput(p.HashOps1, c.threads)
	case "mem_read_bw":
		return p.MemBandwidth(c.workingSet, c.threads, "read")
	case "mem_write_bw":
		return p.MemBandwidth(c.workingSet, c.threads, "write")
	case "mem_copy_bw":
		return p.MemBandwidth(c.workingSet, c.threads, "copy")
	case "mem_latency":
		// A pointer chase is one dependent load at a time; more threads contend slightly
		// for the same cache and memory controller rather than going faster.
		return p.MemLatency(c.workingSet) * (1 + 0.035*float64(c.threads-1))
	case "disk_seq_read_bw":
		return p.DiskSeqBandwidth(p.SeqReadMBs, c.threads)
	case "disk_seq_write_bw":
		return p.DiskSeqBandwidth(p.SeqWriteMBs, c.threads)
	case "disk_rand_read_iops":
		return DiskRandIOPS(p.RandReadIOPS, p.RandReadHalfQD, c.threads)
	case "disk_rand_write_iops":
		return DiskRandIOPS(p.RandWriteIOPS, p.RandWriteHalfQD, c.threads)
	case "disk_rand_read_p99_us":
		return p.DiskReadP99(c.threads)
	default:
		panic("seed: no model for metric " + c.metric)
	}
}

// sigma is the per-trial noise of a configuration. Latency-like metrics are noisier than
// throughput ones, and a p99 is noisier still because it is itself a tail statistic.
func (p Profile) sigma(c config) float64 {
	switch c.metric {
	case "mem_latency":
		return p.NoiseSigma * 1.4
	case "disk_rand_read_p99_us":
		return p.NoiseSigma * 2.2
	case "disk_rand_write_iops":
		return p.NoiseSigma * 0.6
	default:
		return p.NoiseSigma
	}
}

// Generate builds the run envelopes. Every machine's configurations are split across
// RunsPerMachine runs on different days, because a real machine is not swept all at once,
// and that is also what spreads the timestamps over the 30-day window.
func Generate(o Options) []*model.RunEnvelope {
	if o.Machines <= 0 || o.Trials <= 0 {
		return nil
	}
	if o.RunsPerMachine <= 0 {
		o.RunsPerMachine = 1
	}
	now := o.Now
	if now.IsZero() {
		now = time.Now()
	}
	now = now.UTC().Truncate(time.Second)

	profiles := Profiles()
	all := configs()
	var out []*model.RunEnvelope

	for mi := 0; mi < o.Machines; mi++ {
		p := profiles[mi%len(profiles)]
		machine := p.Machine()
		perRun := (len(all) + o.RunsPerMachine - 1) / o.RunsPerMachine

		for ri := 0; ri < o.RunsPerMachine; ri++ {
			start, end := ri*perRun, min((ri+1)*perRun, len(all))
			if start >= end {
				continue
			}
			// Runs are laid out backwards from `now` across 30 days: machine mi, run ri
			// lands on its own day, so the whole set spans the window without collisions.
			daysAgo := 29 - ((mi*o.RunsPerMachine + ri) % 30)
			runStart := now.AddDate(0, 0, -daysAgo).
				Truncate(24 * time.Hour).
				Add(time.Duration(9+ri) * time.Hour)

			env := &model.RunEnvelope{
				SchemaVersion: 1,
				RunID:         runID(o.Seed, mi, ri),
				StartedAt:     stamp(runStart),
				Machine:       machine,
				Argv: []string{"seed", "--machines", fmt.Sprint(o.Machines),
					"--trials", fmt.Sprint(o.Trials), "--seed", fmt.Sprint(o.Seed)},
				Results: make([]model.Result, 0, (end-start)*o.Trials),
			}

			at := runStart
			for ci := start; ci < end; ci++ {
				c := all[ci]
				base := p.base(c)
				sigma := p.sigma(c)
				// One generator per (seed, machine, config): the values of a configuration
				// do not depend on how many other configurations were generated first, so
				// --trials or --machines can change without reshuffling everything else.
				rng := rand.New(rand.NewPCG(o.Seed, uint64(mi)<<32|uint64(ci)))
				params := paramsFor(c, o.Seed)
				for trial := 0; trial < o.Trials; trial++ {
					value := noisy(rng, base, sigma, c.metric)
					durationNs := trialDuration(c.metric)
					at = at.Add(time.Duration(durationNs) + 3*time.Millisecond)
					env.Results = append(env.Results, model.Result{
						Workload:        c.workload,
						ThreadCount:     c.threads,
						WorkingSetBytes: c.workingSet,
						Metric:          c.metric,
						Value:           value,
						Unit:            c.unit,
						Trial:           trial,
						Timestamp:       stamp(at),
						DurationNs:      durationNs,
						Params:          params,
					})
				}
			}
			env.FinishedAt = stamp(at.Add(time.Second))
			out = append(out, env)
		}
	}
	return out
}

// noisy applies log-normal per-trial noise. Log-normal rather than Gaussian because a
// throughput cannot go negative and its distribution is right-skewed: most trials sit near
// the mode, a few are slow. The p99 metric gets a heavier tail on top, since it is the
// statistic that catches stalls.
func noisy(rng *rand.Rand, base, sigma float64, metric string) float64 {
	v := base * math.Exp(rng.NormFloat64()*sigma-sigma*sigma/2)
	if metric == "disk_rand_read_p99_us" && rng.Float64() < 0.04 {
		v *= 1.6 + rng.Float64()*2.4 // an occasional stall, the reason p99 is reported at all
	}
	if metric == "mem_latency" || metric == "disk_rand_read_p99_us" {
		return v // lower is better; no further shaping
	}
	return v
}

func trialDuration(metric string) int64 {
	switch metric {
	case "disk_seq_read_bw", "disk_seq_write_bw", "disk_rand_read_iops",
		"disk_rand_write_iops", "disk_rand_read_p99_us":
		return 500 * 1e6 // the engine's disk default: 500 ms
	default:
		return 50 * 1e6
	}
}

func paramsFor(c config, seed uint64) json.RawMessage {
	p := map[string]any{
		"cold":          "clflush",
		"pinned":        true,
		"warmup_trials": 5,
		"trial_ms":      50,
		"seed":          seed,
	}
	switch c.workload {
	case "cpu_int", "cpu_fp", "cpu_hash":
		p["unit_of_work"] = "ops"
	case "mem_bw":
		p["unit_of_work"] = "bytes"
		p["buffer_bytes"] = c.workingSet
		p["mem_mode"] = map[string]string{
			"mem_read_bw": "read", "mem_write_bw": "write", "mem_copy_bw": "copy",
		}[c.metric]
		p["huge_pages"] = c.workingSet >= 2<<20
	case "mem_latency":
		p["unit_of_work"] = "loads"
		p["chase_lines"] = c.workingSet / 64
		p["line_bytes"] = 64
		p["huge_pages"] = c.workingSet >= 2<<20
	case "disk_seq":
		p["unit_of_work"] = "bytes"
		p["trial_ms"] = 500
		p["block_bytes"] = 1 << 20
		p["queue_depth"] = c.threads
		p["o_direct"] = true
		p["file_bytes"] = c.workingSet
	case "disk_rand":
		p["unit_of_work"] = "ios"
		p["trial_ms"] = 500
		p["block_bytes"] = 4096
		p["queue_depth"] = c.threads
		p["o_direct"] = true
		p["o_dsync"] = c.metric == "disk_rand_write_iops"
		p["file_bytes"] = c.workingSet
	}
	b, err := json.Marshal(p)
	if err != nil {
		panic(err)
	}
	return b
}

// runID is a deterministic UUID v4-shaped id: same seed, same ids, so a re-seeded database
// contains the same runs rather than a second copy of them.
func runID(seed uint64, machine, run int) string {
	rng := rand.New(rand.NewPCG(seed^0x9e3779b97f4a7c15, uint64(machine)<<32|uint64(run)))
	var b [16]byte
	for i := range b {
		b[i] = byte(rng.UintN(256))
	}
	b[6] = (b[6] & 0x0f) | 0x40 // version 4
	b[8] = (b[8] & 0x3f) | 0x80 // variant
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

func stamp(t time.Time) string { return t.UTC().Format("2006-01-02T15:04:05.000000Z") }
