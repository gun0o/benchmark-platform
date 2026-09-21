package store

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"github.com/jackc/pgx/v5"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

// ErrNotFound is returned by the single-object getters.
var ErrNotFound = errors.New("not found")

// MeasurementFilter is the query string of GET /v1/measurements. Nil pointers mean "no
// filter on this column", which is different from a zero value: working_set_bytes = 0 is
// what every CPU metric records.
type MeasurementFilter struct {
	MachineID       string
	RunID           string
	Workload        string
	Metric          string
	ThreadCount     *int
	WorkingSetBytes *int64
	Limit           int
	Cursor          int64 // keyset: return rows with id > Cursor
}

// where builds the shared predicate. Every read endpoint uses it so that the filters mean
// the same thing everywhere and hit the same composite index.
func (f MeasurementFilter) where(args *[]any) string {
	var conds []string
	add := func(sql string, v any) {
		*args = append(*args, v)
		conds = append(conds, fmt.Sprintf(sql, len(*args)))
	}
	if f.MachineID != "" {
		add("machine_id = $%d", f.MachineID)
	}
	if f.Metric != "" {
		add("metric = $%d", f.Metric)
	}
	if f.Workload != "" {
		add("workload = $%d", f.Workload)
	}
	if f.ThreadCount != nil {
		add("thread_count = $%d", *f.ThreadCount)
	}
	if f.WorkingSetBytes != nil {
		add("working_set_bytes = $%d", *f.WorkingSetBytes)
	}
	if f.RunID != "" {
		add("run_id = $%d", f.RunID)
	}
	if len(conds) == 0 {
		return ""
	}
	return " WHERE " + strings.Join(conds, " AND ")
}

// MeasurementPage is one keyset page.
type MeasurementPage struct {
	Measurements []model.Measurement `json:"measurements"`
	NextCursor   string              `json:"next_cursor,omitempty"`
}

// ListMeasurements returns rows ordered by id with keyset pagination (id > cursor), never
// OFFSET: at 100K+ rows an OFFSET page walks every row it skips.
func (s *Store) ListMeasurements(ctx context.Context, f MeasurementFilter) ([]model.Measurement, error) {
	args := make([]any, 0, 8)
	where := f.where(&args)
	if f.Cursor > 0 {
		args = append(args, f.Cursor)
		if where == "" {
			where = fmt.Sprintf(" WHERE id > $%d", len(args))
		} else {
			where += fmt.Sprintf(" AND id > $%d", len(args))
		}
	}
	limit := f.Limit
	if limit <= 0 {
		limit = 100
	}
	args = append(args, limit)
	sql := `SELECT id, run_id, machine_id, workload, metric, thread_count, working_set_bytes,
	               trial, value, unit, recorded_at, duration_ns, params
	        FROM measurements` + where +
		fmt.Sprintf(" ORDER BY id LIMIT $%d", len(args))

	rows, err := s.pool.Query(ctx, sql, args...)
	if err != nil {
		return nil, fmt.Errorf("list measurements: %w", err)
	}
	defer rows.Close()
	out := make([]model.Measurement, 0, limit)
	for rows.Next() {
		var m model.Measurement
		if err := rows.Scan(&m.ID, &m.RunID, &m.MachineID, &m.Workload, &m.Metric,
			&m.ThreadCount, &m.WorkingSetBytes, &m.Trial, &m.Value, &m.Unit,
			&m.RecordedAt, &m.DurationNs, &m.Params); err != nil {
			return nil, fmt.Errorf("scan measurement: %w", err)
		}
		// pgx renders timestamptz into the process's local zone. The instant is right
		// either way, but every timestamp this project writes is UTC with a Z, so the
		// responses say so too rather than carrying the API host's offset.
		m.RecordedAt = m.RecordedAt.UTC()
		out = append(out, m)
	}
	return out, rows.Err()
}

// CountMeasurements is used by the tests and the seed verification.
func (s *Store) CountMeasurements(ctx context.Context, f MeasurementFilter) (int64, error) {
	args := make([]any, 0, 8)
	where := f.where(&args)
	var n int64
	err := s.pool.QueryRow(ctx, `SELECT count(*) FROM measurements`+where, args...).Scan(&n)
	if err != nil {
		return 0, fmt.Errorf("count measurements: %w", err)
	}
	return n, nil
}

const machineCols = `id, hostname, cpu_model, physical_cores, logical_cpus, l1d_kb, l2_kb,
                     l3_kb, memory_bytes, os, kernel, compiler, compiler_flags, virtualized,
                     first_seen, last_seen`

func scanMachine(row pgx.Row, m *model.MachineRow) error {
	if err := row.Scan(&m.ID, &m.Hostname, &m.CPUModel, &m.PhysicalCores, &m.LogicalCPUs,
		&m.L1dKB, &m.L2KB, &m.L3KB, &m.MemoryBytes, &m.OS, &m.Kernel, &m.Compiler,
		&m.CompilerFlags, &m.Virtualized, &m.FirstSeen, &m.LastSeen); err != nil {
		return err
	}
	m.FirstSeen, m.LastSeen = m.FirstSeen.UTC(), m.LastSeen.UTC()
	return nil
}

// ListMachines returns every known machine, newest sighting first, with row counts.
func (s *Store) ListMachines(ctx context.Context) ([]model.MachineRow, error) {
	rows, err := s.pool.Query(ctx, `
		SELECT `+machineCols+`,
		       (SELECT count(*) FROM runs r WHERE r.machine_id = m.id) AS runs,
		       (SELECT count(*) FROM measurements x WHERE x.machine_id = m.id) AS measurements
		FROM machines m ORDER BY last_seen DESC`)
	if err != nil {
		return nil, fmt.Errorf("list machines: %w", err)
	}
	defer rows.Close()
	var out []model.MachineRow
	for rows.Next() {
		var m model.MachineRow
		if err := rows.Scan(&m.ID, &m.Hostname, &m.CPUModel, &m.PhysicalCores, &m.LogicalCPUs,
			&m.L1dKB, &m.L2KB, &m.L3KB, &m.MemoryBytes, &m.OS, &m.Kernel, &m.Compiler,
			&m.CompilerFlags, &m.Virtualized, &m.FirstSeen, &m.LastSeen,
			&m.Runs, &m.Measurements); err != nil {
			return nil, fmt.Errorf("scan machine: %w", err)
		}
		m.FirstSeen, m.LastSeen = m.FirstSeen.UTC(), m.LastSeen.UTC()
		out = append(out, m)
	}
	return out, rows.Err()
}

// GetMachine returns one machine by its content-addressed id.
func (s *Store) GetMachine(ctx context.Context, id string) (model.MachineRow, error) {
	var m model.MachineRow
	row := s.pool.QueryRow(ctx, `SELECT `+machineCols+` FROM machines WHERE id = $1`, id)
	if err := scanMachine(row, &m); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return m, ErrNotFound
		}
		return m, fmt.Errorf("get machine: %w", err)
	}
	return m, nil
}

// ListRuns returns run headers, optionally for one machine, newest first.
func (s *Store) ListRuns(ctx context.Context, machineID string, limit int) ([]model.Run, error) {
	if limit <= 0 {
		limit = 100
	}
	args := []any{limit}
	where := ""
	if machineID != "" {
		args = append(args, machineID)
		where = " WHERE machine_id = $2"
	}
	rows, err := s.pool.Query(ctx, `
		SELECT id, machine_id, engine_version, engine_git_sha, started_at, finished_at,
		       argv, ingested_at
		FROM runs`+where+` ORDER BY started_at DESC LIMIT $1`, args...)
	if err != nil {
		return nil, fmt.Errorf("list runs: %w", err)
	}
	defer rows.Close()
	var out []model.Run
	for rows.Next() {
		var r model.Run
		if err := rows.Scan(&r.ID, &r.MachineID, &r.EngineVersion, &r.EngineGitSHA,
			&r.StartedAt, &r.FinishedAt, &r.Argv, &r.IngestedAt); err != nil {
			return nil, fmt.Errorf("scan run: %w", err)
		}
		r.UTC()
		out = append(out, r)
	}
	return out, rows.Err()
}

// GetRun returns one run header with its measurement count.
func (s *Store) GetRun(ctx context.Context, id string) (model.Run, error) {
	var r model.Run
	err := s.pool.QueryRow(ctx, `
		SELECT id, machine_id, engine_version, engine_git_sha, started_at, finished_at,
		       argv, ingested_at,
		       (SELECT count(*) FROM measurements m WHERE m.run_id = runs.id)
		FROM runs WHERE id = $1`, id).
		Scan(&r.ID, &r.MachineID, &r.EngineVersion, &r.EngineGitSHA, &r.StartedAt,
			&r.FinishedAt, &r.Argv, &r.IngestedAt, &r.Measurements)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return r, ErrNotFound
		}
		return r, fmt.Errorf("get run: %w", err)
	}
	r.UTC()
	return r, nil
}
