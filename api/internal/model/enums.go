// Package model holds the Go shapes of schema/benchmark-result.schema.json.
//
// The schema file is the source of truth. Everything here that duplicates it (the enum
// lists, the metric -> unit and metric -> workload maps) is checked against the schema at
// test time by TestEnumsMatchSchema, so the two cannot drift silently.
package model

// Workloads are the values the `workload` field may take.
var Workloads = []string{
	"cpu_int",
	"cpu_fp",
	"cpu_hash",
	"mem_bw",
	"mem_latency",
	"disk_seq",
	"disk_rand",
}

// Metrics are the values the `metric` field may take, in schema order.
var Metrics = []string{
	"cpu_int_ops",
	"cpu_fp_ops",
	"cpu_hash_ops",
	"mem_read_bw",
	"mem_write_bw",
	"mem_copy_bw",
	"mem_latency",
	"disk_seq_read_bw",
	"disk_seq_write_bw",
	"disk_rand_read_iops",
	"disk_rand_write_iops",
	"disk_rand_read_p99_us",
}

// Units are the values the `unit` field may take.
var Units = []string{"ops/s", "GB/s", "ns", "MB/s", "IOPS", "us"}

// UnitOf pins each metric's unit. The schema encodes the same pairing as if/then blocks.
var UnitOf = map[string]string{
	"cpu_int_ops":           "ops/s",
	"cpu_fp_ops":            "ops/s",
	"cpu_hash_ops":          "ops/s",
	"mem_read_bw":           "GB/s",
	"mem_write_bw":          "GB/s",
	"mem_copy_bw":           "GB/s",
	"mem_latency":           "ns",
	"disk_seq_read_bw":      "MB/s",
	"disk_seq_write_bw":     "MB/s",
	"disk_rand_read_iops":   "IOPS",
	"disk_rand_write_iops":  "IOPS",
	"disk_rand_read_p99_us": "us",
}

// WorkloadOf pins each metric's owning workload, as the schema's if/then blocks do.
var WorkloadOf = map[string]string{
	"cpu_int_ops":           "cpu_int",
	"cpu_fp_ops":            "cpu_fp",
	"cpu_hash_ops":          "cpu_hash",
	"mem_read_bw":           "mem_bw",
	"mem_write_bw":          "mem_bw",
	"mem_copy_bw":           "mem_bw",
	"mem_latency":           "mem_latency",
	"disk_seq_read_bw":      "disk_seq",
	"disk_seq_write_bw":     "disk_seq",
	"disk_rand_read_iops":   "disk_rand",
	"disk_rand_write_iops":  "disk_rand",
	"disk_rand_read_p99_us": "disk_rand",
}

func inList(list []string, v string) bool {
	for _, s := range list {
		if s == v {
			return true
		}
	}
	return false
}

// ValidWorkload reports whether w is in the schema's workload enum.
func ValidWorkload(w string) bool { return inList(Workloads, w) }

// ValidMetric reports whether m is in the schema's metric enum.
func ValidMetric(m string) bool { return inList(Metrics, m) }
