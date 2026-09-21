package store

import (
	"context"
	"encoding/json"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

// copyBatchRows is how many measurement rows go into one CopyFrom call. A 50K-result
// envelope is therefore 5 CopyFrom calls inside one transaction (M5.1): one call would
// hold the whole slice in the protocol buffer at once, and one row per call would be the
// INSERT loop this exists to avoid.
const copyBatchRows = 10_000

// IngestResult is what POST /v1/runs answers with.
type IngestResult struct {
	RunID     string `json:"run_id"`
	Inserted  int    `json:"inserted"`
	Duplicate bool   `json:"duplicate"`
	MachineID string `json:"machine_id"`
}

// IngestRun upserts the machine, inserts the run and bulk-loads its measurements in one
// transaction. It is idempotent on run_id: a run already stored is a no-op that reports
// inserted: 0.
func (s *Store) IngestRun(ctx context.Context, env *model.RunEnvelope) (IngestResult, error) {
	res := IngestResult{RunID: env.RunID, MachineID: env.Machine.ID}
	started, err := model.ParseTimestamp(env.StartedAt)
	if err != nil {
		return res, err
	}
	finished, err := model.ParseTimestamp(env.FinishedAt)
	if err != nil {
		return res, err
	}
	argv, err := json.Marshal(env.Argv)
	if err != nil {
		return res, fmt.Errorf("marshal argv: %w", err)
	}

	err = s.inTx(ctx, func(tx pgx.Tx) error {
		if err := upsertMachine(ctx, tx, &env.Machine, started, finished); err != nil {
			return err
		}
		tag, err := tx.Exec(ctx, `
			INSERT INTO runs (id, machine_id, engine_version, engine_git_sha,
			                  started_at, finished_at, argv)
			VALUES ($1, $2, $3, $4, $5, $6, $7)
			ON CONFLICT (id) DO NOTHING`,
			env.RunID, env.Machine.ID, env.Machine.EngineVersion, env.Machine.EngineGitSHA,
			started, finished, argv)
		if err != nil {
			return fmt.Errorf("insert run: %w", err)
		}
		if tag.RowsAffected() == 0 {
			// Same run_id already stored. The engine retries a POST it could not confirm,
			// so this is a normal path, not an error.
			res.Duplicate = true
			return nil
		}
		n, err := copyMeasurements(ctx, tx, env)
		if err != nil {
			return err
		}
		res.Inserted = n
		return nil
	})
	return res, err
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

var measurementCols = []string{
	"run_id", "machine_id", "workload", "metric", "thread_count", "working_set_bytes",
	"trial", "value", "unit", "recorded_at", "duration_ns", "params",
}

func copyMeasurements(ctx context.Context, tx pgx.Tx, env *model.RunEnvelope) (int, error) {
	total := 0
	for start := 0; start < len(env.Results); start += copyBatchRows {
		end := min(start+copyBatchRows, len(env.Results))
		batch := env.Results[start:end]
		var rowErr error
		i := 0
		src := pgx.CopyFromFunc(func() ([]any, error) {
			if i >= len(batch) || rowErr != nil {
				return nil, rowErr
			}
			r := &batch[i]
			i++
			ts, err := model.ParseTimestamp(r.Timestamp)
			if err != nil {
				rowErr = fmt.Errorf("results[%d].timestamp: %w", start+i-1, err)
				return nil, rowErr
			}
			var params any
			if len(r.Params) > 0 {
				params = []byte(r.Params)
			}
			return []any{
				env.RunID, env.Machine.ID, r.Workload, r.Metric, r.ThreadCount,
				r.WorkingSetBytes, r.Trial, r.Value, r.Unit, ts, r.DurationNs, params,
			}, nil
		})
		n, err := tx.CopyFrom(ctx, pgx.Identifier{"measurements"}, measurementCols, src)
		if err != nil {
			return total, fmt.Errorf("copy measurements: %w", err)
		}
		total += int(n)
	}
	return total, nil
}
