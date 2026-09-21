//go:build integration

package store_test

import (
	"context"
	"encoding/json"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/seed"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

// numpyScript computes the same statistics the SQL does. percentile_cont is numpy's
// "linear" interpolation, stddev_samp is ddof=1, and cov is stddev/mean - so if the two
// disagree, one of them is wrong about a definition, which is exactly what this checks.
const numpyScript = `
import json, sys
import numpy as np
groups = json.load(sys.stdin)
out = {}
for key, values in groups.items():
    a = np.array(values, dtype=np.float64)
    std = float(np.std(a, ddof=1)) if a.size > 1 else 0.0
    mean = float(np.mean(a))
    out[key] = {
        "n": int(a.size),
        "mean": mean,
        "median": float(np.percentile(a, 50, method="linear")),
        "stddev": std,
        "cov": std / mean if mean else 0.0,
        "min": float(np.min(a)),
        "p5": float(np.percentile(a, 5, method="linear")),
        "p95": float(np.percentile(a, 95, method="linear")),
        "max": float(np.max(a)),
    }
json.dump(out, sys.stdout)
`

type numpyGroup struct {
	N      int64   `json:"n"`
	Mean   float64 `json:"mean"`
	Median float64 `json:"median"`
	Stddev float64 `json:"stddev"`
	CoV    float64 `json:"cov"`
	Min    float64 `json:"min"`
	P5     float64 `json:"p5"`
	P95    float64 `json:"p95"`
	Max    float64 `json:"max"`
}

func runNumpy(t *testing.T, groups map[string][]float64) map[string]numpyGroup {
	t.Helper()
	if _, err := exec.LookPath("python3"); err != nil {
		t.Skip("python3 not available")
	}
	if err := exec.Command("python3", "-c", "import numpy").Run(); err != nil {
		t.Skip("numpy not available")
	}
	dir := t.TempDir()
	script := filepath.Join(dir, "agg.py")
	if err := os.WriteFile(script, []byte(numpyScript), 0o600); err != nil {
		t.Fatal(err)
	}
	in, err := json.Marshal(groups)
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command("python3", script)
	cmd.Stdin = bytesReader(in)
	out, err := cmd.Output()
	if err != nil {
		t.Fatalf("numpy: %v", err)
	}
	var res map[string]numpyGroup
	if err := json.Unmarshal(out, &res); err != nil {
		t.Fatalf("numpy output: %v", err)
	}
	return res
}

func bytesReader(b []byte) *os.File {
	r, w, err := os.Pipe()
	if err != nil {
		panic(err)
	}
	go func() {
		defer w.Close()
		_, _ = w.Write(b)
	}()
	return r
}

// TestAggregatesMatchNumpy is the M5.3 verification: the SQL aggregates must equal a numpy
// computation over the very same rows, read back through the list query, to 1e-6 relative.
func TestAggregatesMatchNumpy(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)

	o := seed.Options{Machines: 2, Trials: 60, Seed: 42, RunsPerMachine: 3,
		Now: time.Date(2026, 9, 21, 12, 0, 0, 0, time.UTC)}
	for _, env := range seed.Generate(o) {
		if _, err := s.IngestRun(ctx, env); err != nil {
			t.Fatalf("ingest: %v", err)
		}
	}
	machineID := seed.MachineID(seed.Profiles()[0])

	for _, tc := range []struct {
		metric  string
		groupBy store.GroupBy
		pin     func(*store.MeasurementFilter)
	}{
		{"cpu_int_ops", store.GroupByThreadCount, nil},
		{"mem_read_bw", store.GroupByWorkingSetBytes, func(f *store.MeasurementFilter) {
			n := 4
			f.ThreadCount = &n
		}},
		{"mem_latency", store.GroupByWorkingSetBytes, func(f *store.MeasurementFilter) {
			n := 1
			f.ThreadCount = &n
		}},
		{"disk_rand_read_p99_us", store.GroupByThreadCount, func(f *store.MeasurementFilter) {
			ws := int64(1 << 30)
			f.WorkingSetBytes = &ws
		}},
	} {
		t.Run(tc.metric+"/"+string(tc.groupBy), func(t *testing.T) {
			f := store.MeasurementFilter{MachineID: machineID, Metric: tc.metric}
			if tc.pin != nil {
				tc.pin(&f)
			}

			// The rows the dashboard could fetch itself, through the list endpoint's query.
			raw := map[string][]float64{}
			page := f
			page.Limit = 5000
			for {
				rows, err := s.ListMeasurements(ctx, page)
				if err != nil {
					t.Fatalf("list: %v", err)
				}
				if len(rows) == 0 {
					break
				}
				for _, m := range rows {
					key := itoa(int64(m.ThreadCount))
					if tc.groupBy == store.GroupByWorkingSetBytes {
						key = itoa(m.WorkingSetBytes)
					}
					raw[key] = append(raw[key], m.Value)
				}
				page.Cursor = rows[len(rows)-1].ID
			}
			if len(raw) == 0 {
				t.Fatal("no rows; the comparison would be vacuous")
			}

			got, err := s.Aggregates(ctx, f, tc.groupBy)
			if err != nil {
				t.Fatalf("aggregates: %v", err)
			}
			want := runNumpy(t, raw)
			if len(got.Groups) != len(want) {
				t.Fatalf("SQL returned %d groups, numpy saw %d", len(got.Groups), len(want))
			}

			const tol = 1e-6
			for _, g := range got.Groups {
				w, ok := want[itoa(g.Group)]
				if !ok {
					t.Fatalf("group %d is missing from the numpy side", g.Group)
				}
				if g.N != w.N {
					t.Fatalf("group %d: n %d vs %d", g.Group, g.N, w.N)
				}
				for _, cmp := range []struct {
					name     string
					sql, npy float64
				}{
					{"mean", g.Mean, w.Mean}, {"median", g.Median, w.Median},
					{"stddev", g.Stddev, w.Stddev}, {"cov", g.CoV, w.CoV},
					{"min", g.Min, w.Min}, {"p5", g.P5, w.P5},
					{"p95", g.P95, w.P95}, {"max", g.Max, w.Max},
				} {
					if rel(cmp.sql, cmp.npy) > tol {
						t.Errorf("group %d %s: SQL %.12g, numpy %.12g (relative %.3g)",
							g.Group, cmp.name, cmp.sql, cmp.npy, rel(cmp.sql, cmp.npy))
					}
				}
			}
			t.Logf("%s by %s: %d groups agree with numpy to better than %g",
				tc.metric, tc.groupBy, len(got.Groups), tol)
		})
	}
}

func rel(a, b float64) float64 {
	if a == b {
		return 0
	}
	d := math.Abs(a - b)
	scale := math.Max(math.Abs(a), math.Abs(b))
	if scale == 0 {
		return d
	}
	return d / scale
}

func itoa(v int64) string {
	b, _ := json.Marshal(v)
	return string(b)
}

func TestCompareAlignsSeriesAndComputesRatios(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	o := seed.Options{Machines: 3, Trials: 20, Seed: 42, RunsPerMachine: 2,
		Now: time.Date(2026, 9, 21, 12, 0, 0, 0, time.UTC)}
	for _, env := range seed.Generate(o) {
		if _, err := s.IngestRun(ctx, env); err != nil {
			t.Fatalf("ingest: %v", err)
		}
	}
	ids := []string{
		seed.MachineID(seed.Profiles()[0]),
		seed.MachineID(seed.Profiles()[1]),
		seed.MachineID(seed.Profiles()[2]),
	}
	res, err := s.Compare(ctx, ids, store.MeasurementFilter{Metric: "cpu_int_ops"},
		store.GroupByThreadCount)
	if err != nil {
		t.Fatalf("compare: %v", err)
	}
	if len(res.Series) != 3 {
		t.Fatalf("%d series, want 3", len(res.Series))
	}
	if len(res.Groups) != len(seed.ThreadCounts) {
		t.Fatalf("groups %v, want %v", res.Groups, seed.ThreadCounts)
	}
	for _, s := range res.Series {
		if len(s.Median) != len(res.Groups) || len(s.Ratio) != len(res.Groups) {
			t.Fatalf("series %s is not aligned to the group axis", s.MachineID)
		}
		if s.Hostname == "" {
			t.Fatalf("series %s has no hostname; the dashboard would show a hash", s.MachineID)
		}
	}
	for i, r := range res.Series[0].Ratio {
		if math.Abs(r-1) > 1e-12 {
			t.Fatalf("the baseline's own ratio at group %d is %v, want 1", res.Groups[i], r)
		}
	}
	// The 64-core server must beat the 4-vCPU instance at every thread count.
	for i := range res.Groups {
		if res.Series[2].Ratio[i] >= 1 {
			t.Fatalf("synthetic-03 is not slower than synthetic-01 at %d threads (ratio %.3f)",
				res.Groups[i], res.Series[2].Ratio[i])
		}
	}
}

func TestTrialsAreOrderedAndLimited(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	o := seed.Options{Machines: 1, Trials: 100, Seed: 42, RunsPerMachine: 1,
		Now: time.Date(2026, 9, 21, 12, 0, 0, 0, time.UTC)}
	for _, env := range seed.Generate(o) {
		if _, err := s.IngestRun(ctx, env); err != nil {
			t.Fatalf("ingest: %v", err)
		}
	}
	four := 4
	f := store.MeasurementFilter{
		MachineID: seed.MachineID(seed.Profiles()[0]), Metric: "cpu_int_ops",
		ThreadCount: &four, Limit: 1000,
	}
	points, err := s.Trials(ctx, f)
	if err != nil {
		t.Fatalf("trials: %v", err)
	}
	if len(points) != 100 {
		t.Fatalf("%d trials, want 100", len(points))
	}
	for i := 1; i < len(points); i++ {
		if points[i].RecordedAt.Before(points[i-1].RecordedAt) {
			t.Fatalf("trials are not in time order at %d", i)
		}
	}
	f.Limit = 10
	points, err = s.Trials(ctx, f)
	if err != nil {
		t.Fatalf("trials: %v", err)
	}
	if len(points) != 10 {
		t.Fatalf("limit 10 returned %d", len(points))
	}
}
