package store

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

// copyBatchRows is how many measurement rows go into one CopyFrom call. It bounds the
// memory an ingest holds: results stream out of the JSON decoder into this batch and on
// to Postgres, so a 50K-result envelope is five flushes rather than one 90 MB slice.
const copyBatchRows = 10_000

// IngestResult is what POST /v1/runs answers with.
type IngestResult struct {
	RunID     string `json:"run_id"`
	Results   int    `json:"results"`
	Inserted  int    `json:"inserted"`
	Skipped   int    `json:"skipped"`
	Duplicate bool   `json:"duplicate"`
	MachineID string `json:"machine_id"`
}

var measurementCols = []string{
	"run_id", "machine_id", "workload", "metric", "thread_count", "working_set_bytes",
	"trial", "value", "unit", "recorded_at", "duration_ns", "params",
}

// IngestStream decodes the envelope from body and stores it, holding at most one batch of
// results in memory whatever the envelope's size. Chunks of one run (several POSTs sharing
// a run_id) append; a repeated chunk inserts nothing.
func (s *Store) IngestStream(ctx context.Context, body io.Reader, maxResults int) (IngestResult, error) {
	var ing *ingestion
	_, err := model.StreamRunEnvelope(body, maxResults,
		func(env *model.RunEnvelope, _ int, r *model.Result) error {
			if ing == nil {
				// The first result is where the run becomes real: the header is complete,
				// so the machine and run rows go in and the batches can start flowing.
				var err error
				if ing, err = s.beginIngest(ctx, env); err != nil {
					return err
				}
			}
			return ing.add(ctx, r)
		})
	if err != nil {
		if ing != nil {
			ing.rollback(ctx)
		}
		return IngestResult{}, err
	}
	if ing == nil {
		// StreamRunEnvelope rejects an empty results array, so this cannot normally happen.
		return IngestResult{}, errors.New("run envelope carried no results")
	}
	return ing.commit(ctx)
}

// IngestRun stores an already-decoded envelope. Used by the seeder and the tests; the
// HTTP handler streams instead.
func (s *Store) IngestRun(ctx context.Context, env *model.RunEnvelope) (IngestResult, error) {
	if err := env.Validate(); err != nil {
		return IngestResult{}, err
	}
	if len(env.Results) == 0 {
		return IngestResult{}, errors.New("run envelope carried no results")
	}
	ing, err := s.beginIngest(ctx, env)
	if err != nil {
		return IngestResult{}, err
	}
	for i := range env.Results {
		if err := ing.add(ctx, &env.Results[i]); err != nil {
			ing.rollback(ctx)
			return IngestResult{}, err
		}
	}
	return ing.commit(ctx)
}

// ingestion is one in-flight POST: a transaction, the run it belongs to, and the batch
// being filled.
type ingestion struct {
	tx        pgx.Tx
	runID     string
	machineID string
	rows      [][]any
	inserted  int
	skipped   int
	results   int
	// dedup routes rows through a temp table and ON CONFLICT DO NOTHING instead of copying
	// straight into `measurements`. It is on from the start when the run already existed
	// (a chunk or a retry) and switches on if a plain copy ever hits the unique index.
	dedup     bool
	tempReady bool
}

func (s *Store) beginIngest(ctx context.Context, env *model.RunEnvelope) (*ingestion, error) {
	started, err := model.ParseTimestamp(env.StartedAt)
	if err != nil {
		return nil, err
	}
	finished, err := model.ParseTimestamp(env.FinishedAt)
	if err != nil {
		return nil, err
	}
	argv, err := json.Marshal(env.Argv)
	if err != nil {
		return nil, fmt.Errorf("marshal argv: %w", err)
	}
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return nil, fmt.Errorf("begin: %w", err)
	}
	in := &ingestion{tx: tx, runID: env.RunID, machineID: env.Machine.ID,
		rows: make([][]any, 0, copyBatchRows)}

	if err := upsertMachine(ctx, tx, &env.Machine, started, finished); err != nil {
		in.rollback(ctx)
		return nil, err
	}
	tag, err := tx.Exec(ctx, `
		INSERT INTO runs (id, machine_id, engine_version, engine_git_sha,
		                  started_at, finished_at, argv)
		VALUES ($1, $2, $3, $4, $5, $6, $7)
		ON CONFLICT (id) DO NOTHING`,
		env.RunID, env.Machine.ID, env.Machine.EngineVersion, env.Machine.EngineGitSHA,
		started, finished, argv)
	if err != nil {
		in.rollback(ctx)
		return nil, fmt.Errorf("insert run: %w", err)
	}
	// The run already exists: this POST is either the next chunk of it or a retry of a
	// chunk already stored. Both are handled by the same row-level conflict rule.
	in.dedup = tag.RowsAffected() == 0
	return in, nil
}

func (in *ingestion) add(ctx context.Context, r *model.Result) error {
	ts, err := model.ParseTimestamp(r.Timestamp)
	if err != nil {
		return err
	}
	var params any
	if len(r.Params) > 0 {
		params = []byte(r.Params)
	}
	in.rows = append(in.rows, []any{
		in.runID, in.machineID, r.Workload, r.Metric, r.ThreadCount, r.WorkingSetBytes,
		r.Trial, r.Value, r.Unit, ts, r.DurationNs, params,
	})
	in.results++
	if len(in.rows) >= copyBatchRows {
		return in.flush(ctx)
	}
	return nil
}

func (in *ingestion) flush(ctx context.Context) error {
	if len(in.rows) == 0 {
		return nil
	}
	defer func() { in.rows = in.rows[:0] }()
	if in.dedup {
		return in.flushDedup(ctx)
	}
	// Fast path: this run is new, so nothing in the table can conflict with it - unless
	// the envelope repeats a configuration itself. A savepoint makes that recoverable
	// instead of poisoning the transaction.
	if _, err := in.tx.Exec(ctx, "SAVEPOINT batch"); err != nil {
		return fmt.Errorf("savepoint: %w", err)
	}
	n, err := in.tx.CopyFrom(ctx, pgx.Identifier{"measurements"}, measurementCols,
		pgx.CopyFromRows(in.rows))
	if err != nil {
		var pgErr *pgconn.PgError
		if !errors.As(err, &pgErr) || pgErr.Code != "23505" { // unique_violation
			return fmt.Errorf("copy measurements: %w", err)
		}
		if _, rbErr := in.tx.Exec(ctx, "ROLLBACK TO SAVEPOINT batch"); rbErr != nil {
			return fmt.Errorf("rollback to savepoint: %w", rbErr)
		}
		in.dedup = true // the envelope repeats a configuration; conflict-check from here on
		return in.flushDedup(ctx)
	}
	if _, err := in.tx.Exec(ctx, "RELEASE SAVEPOINT batch"); err != nil {
		return fmt.Errorf("release savepoint: %w", err)
	}
	in.inserted += int(n)
	return nil
}

// flushDedup copies the batch into a temp table and moves it across with ON CONFLICT DO
// NOTHING, because COPY itself cannot express a conflict rule.
func (in *ingestion) flushDedup(ctx context.Context) error {
	if !in.tempReady {
		if _, err := in.tx.Exec(ctx, `
			CREATE TEMP TABLE ingest_batch ON COMMIT DROP AS
			SELECT run_id, machine_id, workload, metric, thread_count, working_set_bytes,
			       trial, value, unit, recorded_at, duration_ns, params
			FROM measurements WITH NO DATA`); err != nil {
			return fmt.Errorf("create temp table: %w", err)
		}
		in.tempReady = true
	}
	if _, err := in.tx.Exec(ctx, `TRUNCATE ingest_batch`); err != nil {
		return fmt.Errorf("truncate temp table: %w", err)
	}
	if _, err := in.tx.CopyFrom(ctx, pgx.Identifier{"ingest_batch"}, measurementCols,
		pgx.CopyFromRows(in.rows)); err != nil {
		return fmt.Errorf("copy into temp table: %w", err)
	}
	tag, err := in.tx.Exec(ctx, `
		INSERT INTO measurements (run_id, machine_id, workload, metric, thread_count,
		                          working_set_bytes, trial, value, unit, recorded_at,
		                          duration_ns, params)
		SELECT run_id, machine_id, workload, metric, thread_count, working_set_bytes,
		       trial, value, unit, recorded_at, duration_ns, params
		FROM ingest_batch
		ON CONFLICT (run_id, metric, thread_count, working_set_bytes, trial) DO NOTHING`)
	if err != nil {
		return fmt.Errorf("insert from temp table: %w", err)
	}
	in.inserted += int(tag.RowsAffected())
	in.skipped += len(in.rows) - int(tag.RowsAffected())
	return nil
}

func (in *ingestion) commit(ctx context.Context) (IngestResult, error) {
	res := IngestResult{RunID: in.runID, MachineID: in.machineID}
	if err := in.flush(ctx); err != nil {
		in.rollback(ctx)
		return res, err
	}
	if err := in.tx.Commit(ctx); err != nil {
		return res, fmt.Errorf("commit: %w", err)
	}
	res.Results = in.results
	res.Inserted = in.inserted
	res.Skipped = in.skipped
	res.Duplicate = in.inserted == 0 && in.results > 0
	return res, nil
}

func (in *ingestion) rollback(ctx context.Context) {
	if in.tx != nil {
		_ = in.tx.Rollback(ctx)
	}
}

func upsertMachine(ctx context.Context, tx pgx.Tx, m *model.Machine, started, finished time.Time) error {
	_, err := tx.Exec(ctx, `
		INSERT INTO machines (id, hostname, cpu_model, physical_cores, logical_cpus,
		                      l1d_kb, l2_kb, l3_kb, memory_bytes, os, kernel, compiler,
		                      compiler_flags, virtualized, first_seen, last_seen)
		VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16)
		ON CONFLICT (id) DO UPDATE SET
			hostname = EXCLUDED.hostname,
			cpu_model = EXCLUDED.cpu_model,
			physical_cores = EXCLUDED.physical_cores,
			logical_cpus = EXCLUDED.logical_cpus,
			l1d_kb = EXCLUDED.l1d_kb,
			l2_kb = EXCLUDED.l2_kb,
			l3_kb = EXCLUDED.l3_kb,
			memory_bytes = EXCLUDED.memory_bytes,
			os = EXCLUDED.os,
			kernel = EXCLUDED.kernel,
			compiler = EXCLUDED.compiler,
			compiler_flags = EXCLUDED.compiler_flags,
			virtualized = EXCLUDED.virtualized,
			first_seen = LEAST(machines.first_seen, EXCLUDED.first_seen),
			last_seen = GREATEST(machines.last_seen, EXCLUDED.last_seen)`,
		m.ID, m.Hostname, m.CPUModel, m.PhysicalCores, m.LogicalCPUs, m.L1dKB, m.L2KB,
		m.L3KB, m.MemoryBytes, m.OS, m.Kernel, m.Compiler, m.CompilerFlags, m.Virtualized,
		started, finished)
	if err != nil {
		return fmt.Errorf("upsert machine: %w", err)
	}
	return nil
}
