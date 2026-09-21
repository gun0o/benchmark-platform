package seed

import "math"

// The value models. Each one is a statement about how a machine behaves, so that a chart
// drawn from seeded data has the same shape as a chart drawn from a measured one: scaling
// that bends, a cache hierarchy with steps, a queue that saturates, a latency tail.

// amdahl is the throughput of n threads given the single-thread value and a serial
// fraction s: T(n) = T1 * n / (1 + (n-1)*s). At s = 0 it is linear; at s = 0.09 (the small
// cloud instance) 16 threads are worth 6.2x, not 16x.
func amdahl(t1 float64, n int, s float64) float64 {
	fn := float64(n)
	return t1 * fn / (1 + (fn-1)*s)
}

// smtPenalty models logical CPUs beyond the physical core count being worth less than a
// core each. Above the logical count, oversubscription costs a little more.
func smtPenalty(n, physical, logical int) float64 {
	switch {
	case n <= physical:
		return 1.0
	case n <= logical:
		over := float64(n-physical) / float64(max(logical-physical, 1))
		return 1.0 - 0.28*over
	default:
		return 0.70
	}
}

// CPUThroughput is the aggregate ops/s of n threads.
func (p Profile) CPUThroughput(base float64, n int) float64 {
	return amdahl(base, n, p.Contention) * smtPenalty(n, p.PhysicalCores, p.LogicalCPUs)
}

// level blends the per-level values of the cache hierarchy at a working set size. The
// blend is a logistic over log2(bytes) rather than a hard step, because a real sweep does
// not fall off a cliff at exactly the cache size: the last points before the boundary are
// already partly missing.
func (p Profile) level(ws int64, l1, l2, l3, dram float64) float64 {
	if ws <= 0 {
		return l1
	}
	lb := math.Log2(float64(ws))
	l1Edge := math.Log2(float64(p.L1dKB) * 1024)
	l2Edge := math.Log2(float64(p.L2KB) * 1024)
	l3Edge := math.Log2(float64(p.L3KB) * 1024)
	// width controls how gradual each transition is, in log2(bytes).
	const width = 0.55
	blend := func(a, b, edge float64) float64 {
		w := 1 / (1 + math.Exp(-(lb-edge)/width)) // 0 below the edge, 1 above
		return a*(1-w) + b*w
	}
	v := blend(l1, l2, l1Edge)
	v = blend(v, l3, l2Edge)
	v = blend(v, dram, l3Edge)
	return v
}

// MemBandwidth is the aggregate GB/s for a working set and thread count: per-thread
// bandwidth from the hierarchy, scaled by threads, capped by the level's shared ceiling.
// The cap is what produces the knee every real bandwidth sweep has.
func (p Profile) MemBandwidth(ws int64, n int, mode string) float64 {
	per := p.level(ws, p.L1BW, p.L2BW, p.L3BW, p.DRAMBW)
	ceiling := p.level(ws, p.L1Ceiling, p.L2Ceiling, p.L3Ceiling, p.DRAMCeiling)
	switch mode {
	case "write":
		// Write-allocate: a store to a line that is not resident fetches it first, so a
		// write costs roughly two transfers once the working set leaves L1.
		resident := 1 / (1 + math.Exp((math.Log2(float64(ws))-math.Log2(float64(p.L1dKB)*1024))/0.55))
		per *= 0.34 + 0.66*resident
		ceiling *= 0.5
	case "copy":
		per *= 0.55
		ceiling *= 0.52
	}
	agg := per * float64(n) * smtPenalty(n, p.PhysicalCores, p.LogicalCPUs)
	return math.Min(agg, ceiling)
}

// MemLatency is nanoseconds per dependent load at a working set: the same blended
// hierarchy, with a small penalty for the TLB pressure of very large sets.
func (p Profile) MemLatency(ws int64) float64 {
	lat := p.level(ws, p.L1Lat, p.L2Lat, p.L3Lat, p.DRAMLat)
	if ws > int64(p.L3KB)*1024 {
		// Latency keeps creeping up past the last-level cache, as M3.2 measured: +20% from
		// 24 MiB to 1 GiB on the real machine.
		decades := math.Log2(float64(ws)/(float64(p.L3KB)*1024)) / 10
		lat *= 1 + 0.20*math.Min(decades, 1)
	}
	return lat
}

// DiskSeqBandwidth saturates with queue depth: one stream reaches a good fraction of the
// device, a few more reach the ceiling, and more than that adds nothing.
func (p Profile) DiskSeqBandwidth(ceiling float64, qd int) float64 {
	q := float64(qd)
	return ceiling * (0.42 + 0.58*q/(q+2.2))
}

// DiskRandIOPS is Little's law with a ceiling: IOPS = cap * qd / (qd + halfQD).
func DiskRandIOPS(cap, halfQD float64, qd int) float64 {
	q := float64(qd)
	return cap * q / (q + halfQD)
}

// DiskReadP99 grows with queue depth, because a deeper queue means more waiting behind
// other I/Os for the unlucky request.
func (p Profile) DiskReadP99(qd int) float64 {
	return p.ReadP99Us * (0.75 + 0.25*float64(qd))
}
