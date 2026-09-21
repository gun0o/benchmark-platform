package lttb_test

import (
	"math"
	"testing"

	"github.com/matthewlee/benchmark-platform/api/internal/lttb"
)

func series(n int, f func(i int) float64) []lttb.Point {
	out := make([]lttb.Point, n)
	for i := range out {
		out[i] = lttb.Point{X: float64(i), Y: f(i)}
	}
	return out
}

func TestReturnsInputWhenNotWorthDownsampling(t *testing.T) {
	in := series(50, func(i int) float64 { return float64(i) })
	for _, threshold := range []int{0, 2, 50, 100} {
		if got := lttb.Downsample(in, threshold); len(got) != len(in) {
			t.Fatalf("threshold %d: %d points, want %d", threshold, len(got), len(in))
		}
	}
}

func TestKeepsEndpointsAndCount(t *testing.T) {
	in := series(10_000, func(i int) float64 { return math.Sin(float64(i) / 200) })
	out := lttb.Downsample(in, 2000)
	if len(out) != 2000 {
		t.Fatalf("%d points, want 2000", len(out))
	}
	if out[0] != in[0] || out[len(out)-1] != in[len(in)-1] {
		t.Fatal("first and last points must survive")
	}
	for i := 1; i < len(out); i++ {
		if out[i].X <= out[i-1].X {
			t.Fatalf("output is not in x order at %d", i)
		}
	}
}

// The property that matters: a spike must survive. Decimation by every-Nth would drop it
// four times out of five.
func TestKeepsASpike(t *testing.T) {
	in := series(10_000, func(i int) float64 { return 100 })
	in[4137].Y = 900 // one slow trial, the kind a variance study exists to find
	out := lttb.Downsample(in, 500)
	found := false
	for _, p := range out {
		if p.Y == 900 {
			found = true
		}
	}
	if !found {
		t.Fatal("LTTB dropped the only outlier in the series")
	}
}

func TestPreservesRangeApproximately(t *testing.T) {
	in := series(10_000, func(i int) float64 {
		return 100 + 20*math.Sin(float64(i)/97) + 5*math.Cos(float64(i)/7)
	})
	out := lttb.Downsample(in, 1000)
	minIn, maxIn := extremes(in)
	minOut, maxOut := extremes(out)
	if maxOut < maxIn*0.99 || minOut > minIn*1.01 {
		t.Fatalf("range shrank: [%.2f, %.2f] -> [%.2f, %.2f]", minIn, maxIn, minOut, maxOut)
	}
}

func extremes(p []lttb.Point) (lo, hi float64) {
	lo, hi = math.Inf(1), math.Inf(-1)
	for _, v := range p {
		lo = math.Min(lo, v.Y)
		hi = math.Max(hi, v.Y)
	}
	return lo, hi
}
