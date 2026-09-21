// Package seed generates synthetic benchmark runs that are shaped like real ones.
//
// The point is not volume for its own sake: the dashboard has to be developed against
// data whose curves mean something, so the values come from a model of how a machine
// behaves (Amdahl-like scaling, a cache hierarchy, a saturating queue) with per-trial
// noise, not from uniform random numbers. Everything is deterministic given --seed.
//
// Synthetic machines are labelled as such - hostname "synthetic-NN", engine_version
// "seed" - so they can always be told apart from measured data.
package seed

import (
	"crypto/sha256"
	"encoding/hex"
	"fmt"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

// Profile is the physical description of one fictional machine plus the parameters of the
// models that generate its numbers.
type Profile struct {
	Hostname      string
	CPUModel      string
	PhysicalCores int
	LogicalCPUs   int
	L1dKB         int
	L2KB          int
	L3KB          int
	MemoryBytes   int64
	OS            string
	Kernel        string
	Compiler      string
	CompilerFlags string
	Virtualized   string

	// CPU: single-thread throughput per metric and the serial fraction that bends the
	// scaling curve, T(n) = T1 * n / (1 + (n-1)*s).
	IntOps1    float64
	FPOps1     float64
	HashOps1   float64
	Contention float64 // s, 0 = perfect scaling
	NoiseSigma float64 // log-normal per-trial noise, as a fraction (0.01 - 0.04)
	ClockGHz   float64

	// Memory: per-thread bandwidth at each level of the hierarchy (GB/s) and the shared
	// ceiling each level saturates at.
	L1BW, L2BW, L3BW, DRAMBW                     float64
	L1Ceiling, L2Ceiling, L3Ceiling, DRAMCeiling float64
	// Latency at each level, ns.
	L1Lat, L2Lat, L3Lat, DRAMLat float64

	// Disk: sequential ceilings (MB/s), random IOPS ceilings and the queue depth at which
	// half the ceiling is reached.
	SeqReadMBs, SeqWriteMBs float64
	RandReadIOPS            float64
	RandReadHalfQD          float64
	RandWriteIOPS           float64
	RandWriteHalfQD         float64
	ReadP99Us               float64
}

// MachineID is the engine's rule: SHA-256 over
// cpu_model|physical_cores|logical_cpus|l3_kb|memory_bytes|hostname, first 16 bytes.
// The seeder uses the same rule so a synthetic machine is addressed like a real one.
func MachineID(p Profile) string {
	sum := sha256.Sum256([]byte(fmt.Sprintf("%s|%d|%d|%d|%d|%s",
		p.CPUModel, p.PhysicalCores, p.LogicalCPUs, p.L3KB, p.MemoryBytes, p.Hostname)))
	return hex.EncodeToString(sum[:16])
}

// Machine renders the profile as the machine block of an envelope.
func (p Profile) Machine() model.Machine {
	return model.Machine{
		ID:            MachineID(p),
		Hostname:      p.Hostname,
		CPUModel:      p.CPUModel,
		PhysicalCores: p.PhysicalCores,
		LogicalCPUs:   p.LogicalCPUs,
		L1dKB:         p.L1dKB,
		L2KB:          p.L2KB,
		L3KB:          p.L3KB,
		MemoryBytes:   p.MemoryBytes,
		OS:            p.OS,
		Kernel:        p.Kernel,
		Compiler:      p.Compiler,
		CompilerFlags: p.CompilerFlags,
		EngineVersion: "seed",
		EngineGitSHA:  "synthetic",
		Virtualized:   p.Virtualized,
	}
}

// Profiles are five deliberately different machines: a big server part, a laptop, a small
// cloud instance, an older desktop and a hybrid-core laptop like the one this project is
// measured on. They differ in core count, cache sizes, memory bandwidth, disk class and
// noise, so every chart in the dashboard has something to compare.
func Profiles() []Profile {
	return []Profile{
		{
			Hostname: "synthetic-01", CPUModel: "AMD EPYC 9554 64-Core Processor",
			PhysicalCores: 64, LogicalCPUs: 128, L1dKB: 32, L2KB: 1024, L3KB: 262144,
			MemoryBytes: 549755813888, OS: "Ubuntu 24.04.1 LTS", Kernel: "6.8.0-45-generic",
			Compiler: "g++ 13.2.0", CompilerFlags: "-O3 -march=native", Virtualized: "none",
			IntOps1: 3.9e9, FPOps1: 7.8e9, HashOps1: 1.3e8, Contention: 0.004,
			NoiseSigma: 0.010, ClockGHz: 3.75,
			L1BW: 210, L2BW: 155, L3BW: 72, DRAMBW: 24,
			L1Ceiling: 5000, L2Ceiling: 3600, L3Ceiling: 1400, DRAMCeiling: 420,
			L1Lat: 0.9, L2Lat: 3.1, L3Lat: 17.0, DRAMLat: 92,
			SeqReadMBs: 6800, SeqWriteMBs: 5200, RandReadIOPS: 780000, RandReadHalfQD: 12,
			RandWriteIOPS: 240000, RandWriteHalfQD: 8, ReadP99Us: 95,
		},
		{
			Hostname: "synthetic-02", CPUModel: "Intel(R) Core(TM) i7-1370P",
			PhysicalCores: 14, LogicalCPUs: 20, L1dKB: 48, L2KB: 2048, L3KB: 24576,
			MemoryBytes: 34359738368, OS: "Fedora 40", Kernel: "6.10.9-200.fc40.x86_64",
			Compiler: "g++ 14.2.1", CompilerFlags: "-O3 -march=native", Virtualized: "none",
			IntOps1: 4.9e9, FPOps1: 9.6e9, HashOps1: 1.6e8, Contention: 0.035,
			NoiseSigma: 0.025, ClockGHz: 4.9,
			L1BW: 265, L2BW: 190, L3BW: 84, DRAMBW: 23,
			L1Ceiling: 1500, L2Ceiling: 900, L3Ceiling: 300, DRAMCeiling: 88,
			L1Lat: 1.0, L2Lat: 3.3, L3Lat: 21.0, DRAMLat: 108,
			SeqReadMBs: 3400, SeqWriteMBs: 2600, RandReadIOPS: 190000, RandReadHalfQD: 9,
			RandWriteIOPS: 62000, RandWriteHalfQD: 6, ReadP99Us: 140,
		},
		{
			Hostname: "synthetic-03", CPUModel: "Intel(R) Xeon(R) Platinum 8375C (4 vCPU)",
			PhysicalCores: 2, LogicalCPUs: 4, L1dKB: 48, L2KB: 1280, L3KB: 55296,
			MemoryBytes: 8589934592, OS: "Amazon Linux 2023", Kernel: "6.1.109-118.189.amzn2023",
			Compiler: "g++ 11.4.1", CompilerFlags: "-O3 -march=x86-64-v3", Virtualized: "kvm",
			IntOps1: 3.1e9, FPOps1: 6.2e9, HashOps1: 1.0e8, Contention: 0.090,
			NoiseSigma: 0.040, ClockGHz: 2.9,
			L1BW: 150, L2BW: 105, L3BW: 48, DRAMBW: 15,
			L1Ceiling: 420, L2Ceiling: 260, L3Ceiling: 120, DRAMCeiling: 38,
			L1Lat: 1.4, L2Lat: 4.6, L3Lat: 26.0, DRAMLat: 135,
			SeqReadMBs: 1250, SeqWriteMBs: 640, RandReadIOPS: 64000, RandReadHalfQD: 7,
			RandWriteIOPS: 21000, RandWriteHalfQD: 5, ReadP99Us: 310,
		},
		{
			Hostname: "synthetic-04", CPUModel: "Intel(R) Core(TM) i5-6500",
			PhysicalCores: 4, LogicalCPUs: 4, L1dKB: 32, L2KB: 256, L3KB: 6144,
			MemoryBytes: 17179869184, OS: "Debian GNU/Linux 12", Kernel: "6.1.0-25-amd64",
			Compiler: "g++ 12.2.0", CompilerFlags: "-O3 -march=native", Virtualized: "none",
			IntOps1: 2.4e9, FPOps1: 4.1e9, HashOps1: 6.5e7, Contention: 0.020,
			NoiseSigma: 0.015, ClockGHz: 3.2,
			L1BW: 98, L2BW: 62, L3BW: 33, DRAMBW: 12,
			L1Ceiling: 390, L2Ceiling: 240, L3Ceiling: 120, DRAMCeiling: 30,
			L1Lat: 1.3, L2Lat: 3.8, L3Lat: 13.0, DRAMLat: 74,
			SeqReadMBs: 520, SeqWriteMBs: 480, RandReadIOPS: 92000, RandReadHalfQD: 8,
			RandWriteIOPS: 28000, RandWriteHalfQD: 6, ReadP99Us: 220,
		},
		{
			Hostname: "synthetic-05", CPUModel: "Intel(R) Core(TM) Ultra 7 155H",
			PhysicalCores: 11, LogicalCPUs: 22, L1dKB: 48, L2KB: 2048, L3KB: 24576,
			MemoryBytes: 16659374080, OS: "Ubuntu 22.04.5 LTS (WSL2)",
			Kernel: "6.18.33.2-microsoft-standard-WSL2", Compiler: "g++ 13.4.0",
			CompilerFlags: "-O3 -march=native", Virtualized: "wsl2",
			IntOps1: 4.3e9, FPOps1: 8.4e9, HashOps1: 1.4e8, Contention: 0.055,
			NoiseSigma: 0.035, ClockGHz: 4.5,
			L1BW: 240, L2BW: 175, L3BW: 70, DRAMBW: 21,
			L1Ceiling: 1300, L2Ceiling: 800, L3Ceiling: 260, DRAMCeiling: 80,
			L1Lat: 1.05, L2Lat: 3.4, L3Lat: 46.0, DRAMLat: 160,
			SeqReadMBs: 2650, SeqWriteMBs: 1480, RandReadIOPS: 90000, RandReadHalfQD: 10,
			RandWriteIOPS: 600, RandWriteHalfQD: 4, ReadP99Us: 275,
		},
	}
}
