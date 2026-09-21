package model

import (
	"encoding/json"
	"fmt"
	"regexp"
	"strings"
	"time"
)

// Machine is the machine block, hoisted to the top of a run envelope.
type Machine struct {
	ID            string `json:"id"`
	Hostname      string `json:"hostname"`
	CPUModel      string `json:"cpu_model"`
	PhysicalCores int    `json:"physical_cores"`
	LogicalCPUs   int    `json:"logical_cpus"`
	L1dKB         int    `json:"l1d_kb"`
	L2KB          int    `json:"l2_kb"`
	L3KB          int    `json:"l3_kb"`
	MemoryBytes   int64  `json:"memory_bytes"`
	OS            string `json:"os"`
	Kernel        string `json:"kernel"`
	Compiler      string `json:"compiler"`
	CompilerFlags string `json:"compiler_flags"`
	EngineVersion string `json:"engine_version"`
	EngineGitSHA  string `json:"engine_git_sha"`
	Virtualized   string `json:"virtualized,omitempty"`
}

// Result is one trial of one metric for one configuration, as it appears inside a run
// envelope (no machine block: it is hoisted).
type Result struct {
	Workload        string          `json:"workload"`
	ThreadCount     int             `json:"thread_count"`
	WorkingSetBytes int64           `json:"working_set_bytes"`
	Metric          string          `json:"metric"`
	Value           float64         `json:"value"`
	Unit            string          `json:"unit"`
	Trial           int             `json:"trial"`
	Timestamp       string          `json:"timestamp"`
	DurationNs      int64           `json:"duration_ns"`
	Params          json.RawMessage `json:"params,omitempty"`
}

// Summary is one per-configuration summary the engine computed over the trials it ran.
// The API stores runs and measurements; summaries are recomputed server-side (M5.3), so
// this type exists to accept and validate the field, not to persist it.
type Summary struct {
	Workload        string  `json:"workload"`
	Metric          string  `json:"metric"`
	ThreadCount     int     `json:"thread_count"`
	WorkingSetBytes int64   `json:"working_set_bytes"`
	N               int     `json:"n"`
	Mean            float64 `json:"mean"`
	Median          float64 `json:"median"`
	Stddev          float64 `json:"stddev"`
	CoV             float64 `json:"cov"`
	Min             float64 `json:"min"`
	P5              float64 `json:"p5"`
	P95             float64 `json:"p95"`
	Max             float64 `json:"max"`
	MAD             float64 `json:"mad,omitempty"`
	// M2.5 run-quality fields. They describe the conditions the trials were taken under,
	// never the trials themselves. The API accepts them so that strict decoding of a real
	// engine envelope succeeds; it does not store them, because it recomputes summaries
	// from the measurements (M5.3).
	LateTrials       int     `json:"late_trials,omitempty"`
	CanaryAttempts   int     `json:"canary_attempts,omitempty"`
	ClockCallNsStart float64 `json:"clock_call_ns_start,omitempty"`
	ClockCallNsEnd   float64 `json:"clock_call_ns_end,omitempty"`
}

// RunEnvelope is what `bench run` writes and POST /v1/runs accepts.
type RunEnvelope struct {
	SchemaVersion int       `json:"schema_version"`
	RunID         string    `json:"run_id"`
	StartedAt     string    `json:"started_at"`
	FinishedAt    string    `json:"finished_at"`
	Machine       Machine   `json:"machine"`
	Argv          []string  `json:"argv"`
	Results       []Result  `json:"results"`
	Summary       []Summary `json:"summary,omitempty"`
}

// Measurement is one stored row, the canonical shape the read endpoints return.
type Measurement struct {
	ID              int64           `json:"id"`
	RunID           string          `json:"run_id"`
	MachineID       string          `json:"machine_id"`
	Workload        string          `json:"workload"`
	Metric          string          `json:"metric"`
	ThreadCount     int             `json:"thread_count"`
	WorkingSetBytes int64           `json:"working_set_bytes"`
	Trial           int             `json:"trial"`
	Value           float64         `json:"value"`
	Unit            string          `json:"unit"`
	RecordedAt      time.Time       `json:"recorded_at"`
	DurationNs      int64           `json:"duration_ns"`
	Params          json.RawMessage `json:"params,omitempty"`
}

// Run is a stored run header.
type Run struct {
	ID            string          `json:"id"`
	MachineID     string          `json:"machine_id"`
	EngineVersion string          `json:"engine_version"`
	EngineGitSHA  string          `json:"engine_git_sha"`
	StartedAt     time.Time       `json:"started_at"`
	FinishedAt    time.Time       `json:"finished_at"`
	Argv          json.RawMessage `json:"argv"`
	IngestedAt    time.Time       `json:"ingested_at"`
	Measurements  int64           `json:"measurements,omitempty"`
}

// UTC normalizes a run's timestamps. pgx renders timestamptz in the process's local zone,
// and every timestamp this project writes is UTC with a Z, so the responses say so too.
func (r *Run) UTC() {
	r.StartedAt, r.FinishedAt, r.IngestedAt = r.StartedAt.UTC(), r.FinishedAt.UTC(), r.IngestedAt.UTC()
}

// MachineRow is a stored machine with its first/last sighting.
type MachineRow struct {
	Machine
	FirstSeen    time.Time `json:"first_seen"`
	LastSeen     time.Time `json:"last_seen"`
	Runs         int64     `json:"runs,omitempty"`
	Measurements int64     `json:"measurements,omitempty"`
}

var (
	uuidRe      = regexp.MustCompile(`^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$`)
	machineIDRe = regexp.MustCompile(`^[0-9a-f]{32}$`)
	// The schema's timestamp pattern: RFC 3339 in UTC with a Z suffix.
	timestampRe = regexp.MustCompile(`^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,9})?Z$`)
)

// TimestampLayouts is what ParseTimestamp accepts, in order.
const timestampLayout = "2006-01-02T15:04:05.999999999Z"

// ParseTimestamp parses one of the schema's timestamps into UTC.
func ParseTimestamp(s string) (time.Time, error) {
	if !timestampRe.MatchString(s) {
		return time.Time{}, fmt.Errorf("timestamp %q is not RFC 3339 UTC with a Z suffix", s)
	}
	t, err := time.Parse(timestampLayout, s)
	if err != nil {
		return time.Time{}, fmt.Errorf("timestamp %q: %w", s, err)
	}
	return t.UTC(), nil
}

// ValidationError lists everything wrong with a document. It carries every problem rather
// than the first, because a client fixing a 50K-result envelope should not have to POST it
// twelve times to find twelve mistakes.
type ValidationError struct {
	Problems []string
}

func (e *ValidationError) Error() string {
	if len(e.Problems) == 1 {
		return e.Problems[0]
	}
	return fmt.Sprintf("%d problems: %s", len(e.Problems), strings.Join(e.Problems, "; "))
}

const maxProblems = 20

type problems struct{ list []string }

func (p *problems) addf(format string, args ...any) {
	if len(p.list) < maxProblems {
		p.list = append(p.list, fmt.Sprintf(format, args...))
	}
}

// ValidateMachine checks the machine block's required fields and patterns.
func (m *Machine) validate(p *problems) {
	if !machineIDRe.MatchString(m.ID) {
		p.addf("machine.id %q is not 32 hex characters", m.ID)
	}
	for _, f := range []struct{ name, val string }{
		{"hostname", m.Hostname}, {"cpu_model", m.CPUModel}, {"os", m.OS},
		{"kernel", m.Kernel}, {"compiler", m.Compiler},
		{"engine_version", m.EngineVersion}, {"engine_git_sha", m.EngineGitSHA},
	} {
		if f.val == "" {
			p.addf("machine.%s is required and must be non-empty", f.name)
		}
	}
	if m.PhysicalCores < 1 {
		p.addf("machine.physical_cores must be >= 1, got %d", m.PhysicalCores)
	}
	if m.LogicalCPUs < 1 {
		p.addf("machine.logical_cpus must be >= 1, got %d", m.LogicalCPUs)
	}
	for _, f := range []struct {
		name string
		val  int
	}{{"l1d_kb", m.L1dKB}, {"l2_kb", m.L2KB}, {"l3_kb", m.L3KB}} {
		if f.val < 0 {
			p.addf("machine.%s must be >= 0, got %d", f.name, f.val)
		}
	}
	if m.MemoryBytes < 0 {
		p.addf("machine.memory_bytes must be >= 0, got %d", m.MemoryBytes)
	}
}

// Validate checks one result against the schema's rules, including the metric <-> unit and
// metric <-> workload consistency the schema expresses as if/then blocks.
func (r *Result) validate(i int, p *problems) {
	at := func(f string) string { return fmt.Sprintf("results[%d].%s", i, f) }
	if !ValidWorkload(r.Workload) {
		p.addf("%s: unknown workload %q", at("workload"), r.Workload)
	}
	if !ValidMetric(r.Metric) {
		p.addf("%s: unknown metric %q", at("metric"), r.Metric)
		return // the consistency checks below have nothing to check against
	}
	if want := WorkloadOf[r.Metric]; r.Workload != want {
		p.addf("%s: metric %s belongs to workload %s, got %q", at("workload"), r.Metric, want, r.Workload)
	}
	if want := UnitOf[r.Metric]; r.Unit != want {
		p.addf("%s: metric %s is measured in %s, got %q", at("unit"), r.Metric, want, r.Unit)
	}
	if r.ThreadCount < 1 {
		p.addf("%s: must be >= 1, got %d", at("thread_count"), r.ThreadCount)
	}
	if r.WorkingSetBytes < 0 {
		p.addf("%s: must be >= 0, got %d", at("working_set_bytes"), r.WorkingSetBytes)
	}
	if r.Trial < 0 {
		p.addf("%s: must be >= 0, got %d", at("trial"), r.Trial)
	}
	if r.DurationNs < 1 {
		p.addf("%s: must be >= 1, got %d", at("duration_ns"), r.DurationNs)
	}
	if _, err := ParseTimestamp(r.Timestamp); err != nil {
		p.addf("%s: %v", at("timestamp"), err)
	}
	// The schema says "number", which in JSON cannot be NaN or Inf; Go's decoder rejects
	// those tokens too, so a non-finite value can only arrive here from another Go caller.
	if isNaN(r.Value) || isInf(r.Value) {
		p.addf("%s: must be finite, got %v", at("value"), r.Value)
	}
}

// Validate checks a whole envelope. It returns *ValidationError with every problem found
// (capped), or nil.
func (e *RunEnvelope) Validate() error {
	var p problems
	if e.SchemaVersion != 1 {
		p.addf("schema_version must be 1, got %d", e.SchemaVersion)
	}
	if !uuidRe.MatchString(e.RunID) {
		p.addf("run_id %q is not a UUID v4", e.RunID)
	}
	started, err := ParseTimestamp(e.StartedAt)
	if err != nil {
		p.addf("started_at: %v", err)
	}
	finished, err := ParseTimestamp(e.FinishedAt)
	if err != nil {
		p.addf("finished_at: %v", err)
	}
	if err == nil && !started.IsZero() && finished.Before(started) {
		p.addf("finished_at %s is before started_at %s", e.FinishedAt, e.StartedAt)
	}
	e.Machine.validate(&p)
	for i := range e.Results {
		e.Results[i].validate(i, &p)
	}
	if len(p.list) > 0 {
		return &ValidationError{Problems: p.list}
	}
	return nil
}

func isNaN(f float64) bool { return f != f }
func isInf(f float64) bool {
	return f > 1.797693134862315708145274237317043567981e+308 || f < -1.797693134862315708145274237317043567981e+308
}
