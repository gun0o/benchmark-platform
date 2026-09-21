//go:build integration

package store_test

import (
	"context"
	"fmt"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

// TestIngestPathTimings measures the three ways 50,000 measurement rows can reach
// Postgres, so the choice in ingest.go is a measurement rather than folklore. It prints a
// table; the only thing it asserts is the ordering the design depends on.
//
//	go test -tags integration -run IngestPathTimings -v ./internal/store/
func TestIngestPathTimings(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	const n = 50_000
	const insertLoopRows = 5_000 // the row-by-row path is too slow to run 50K of

	env := syntheticEnvelope("88888888-9999-4aaa-8bbb-cccccccccccc", n)
	rows := make([][]any, 0, n)
	base := time.Date(2026, 9, 21, 12, 0, 0, 0, time.UTC)
	for i := range env.Results {
		r := &env.Results[i]
		rows = append(rows, []any{
			env.RunID, env.Machine.ID, r.Workload, r.Metric, r.ThreadCount,
			r.WorkingSetBytes, r.Trial, r.Value, r.Unit, base.Add(time.Duration(i)),
			r.DurationNs, []byte(r.Params),
		})
	}
	cols := []string{"run_id", "machine_id", "workload", "metric", "thread_count",
		"working_set_bytes", "trial", "value", "unit", "recorded_at", "duration_ns", "params"}

	// The rows reference a run and a machine, so those have to exist first.
	if _, err := s.IngestRun(ctx, syntheticEnvelope(env.RunID, 1)); err != nil {
		t.Fatalf("seed run: %v", err)
	}
	clear := func() {
		if _, err := s.Pool().Exec(ctx, `DELETE FROM measurements`); err != nil {
			t.Fatalf("clear: %v", err)
		}
	}

	type timing struct {
		name    string
		rows    int
		elapsed time.Duration
	}
	var results []timing
	measure := func(name string, count int, fn func(pgx.Tx) error) {
		clear()
		tx, err := s.Pool().Begin(ctx)
		if err != nil {
			t.Fatalf("begin: %v", err)
		}
		start := time.Now()
		if err := fn(tx); err != nil {
			_ = tx.Rollback(ctx)
			t.Fatalf("%s: %v", name, err)
		}
		if err := tx.Commit(ctx); err != nil {
			t.Fatalf("%s commit: %v", name, err)
		}
		results = append(results, timing{name, count, time.Since(start)})
	}

	measure(fmt.Sprintf("row-by-row INSERT (%d rows)", insertLoopRows), insertLoopRows,
		func(tx pgx.Tx) error {
			for _, r := range rows[:insertLoopRows] {
				if _, err := tx.Exec(ctx, `
					INSERT INTO measurements (run_id, machine_id, workload, metric,
						thread_count, working_set_bytes, trial, value, unit, recorded_at,
						duration_ns, params)
					VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)`, r...); err != nil {
					return err
				}
			}
			return nil
		})

	measure(fmt.Sprintf("CopyFrom, one batch (%d rows)", n), n, func(tx pgx.Tx) error {
		_, err := tx.CopyFrom(ctx, pgx.Identifier{"measurements"}, cols, pgx.CopyFromRows(rows))
		return err
	})

	measure(fmt.Sprintf("CopyFrom, 10K batches (%d rows)", n), n, func(tx pgx.Tx) error {
		for start := 0; start < len(rows); start += 10_000 {
			end := min(start+10_000, len(rows))
			if _, err := tx.CopyFrom(ctx, pgx.Identifier{"measurements"}, cols,
				pgx.CopyFromRows(rows[start:end])); err != nil {
				return err
			}
		}
		return nil
	})

	measure(fmt.Sprintf("temp table + ON CONFLICT, 10K batches (%d rows)", n), n,
		func(tx pgx.Tx) error {
			if _, err := tx.Exec(ctx, `
				CREATE TEMP TABLE bench_batch ON COMMIT DROP AS
				SELECT run_id, machine_id, workload, metric, thread_count, working_set_bytes,
				       trial, value, unit, recorded_at, duration_ns, params
				FROM measurements WITH NO DATA`); err != nil {
				return err
			}
			for start := 0; start < len(rows); start += 10_000 {
				end := min(start+10_000, len(rows))
				if _, err := tx.Exec(ctx, `TRUNCATE bench_batch`); err != nil {
					return err
				}
				if _, err := tx.CopyFrom(ctx, pgx.Identifier{"bench_batch"}, cols,
					pgx.CopyFromRows(rows[start:end])); err != nil {
					return err
				}
				if _, err := tx.Exec(ctx, `
					INSERT INTO measurements (run_id, machine_id, workload, metric, thread_count,
						working_set_bytes, trial, value, unit, recorded_at, duration_ns, params)
					SELECT run_id, machine_id, workload, metric, thread_count, working_set_bytes,
					       trial, value, unit, recorded_at, duration_ns, params
					FROM bench_batch
					ON CONFLICT (run_id, metric, thread_count, working_set_bytes, trial)
					DO NOTHING`); err != nil {
					return err
				}
			}
			return nil
		})

	// Two diagnostics, to say where the COPY time actually goes.
	noParams := make([][]any, len(rows))
	for i, r := range rows {
		cp := append([]any(nil), r...)
		cp[len(cp)-1] = nil
		noParams[i] = cp
	}
	measure(fmt.Sprintf("CopyFrom, 10K batches, params NULL (%d rows)", n), n, func(tx pgx.Tx) error {
		for start := 0; start < len(noParams); start += 10_000 {
			end := min(start+10_000, len(noParams))
			if _, err := tx.CopyFrom(ctx, pgx.Identifier{"measurements"}, cols,
				pgx.CopyFromRows(noParams[start:end])); err != nil {
				return err
			}
		}
		return nil
	})

	clear()
	for _, idx := range []string{"measurements_dedup_idx", "measurements_filter_idx", "measurements_run_idx"} {
		if _, err := s.Pool().Exec(ctx, `DROP INDEX IF EXISTS `+idx); err != nil {
			t.Fatalf("drop %s: %v", idx, err)
		}
	}
	measure(fmt.Sprintf("CopyFrom, 10K batches, no indexes (%d rows)", n), n, func(tx pgx.Tx) error {
		for start := 0; start < len(rows); start += 10_000 {
			end := min(start+10_000, len(rows))
			if _, err := tx.CopyFrom(ctx, pgx.Identifier{"measurements"}, cols,
				pgx.CopyFromRows(rows[start:end])); err != nil {
				return err
			}
		}
		return nil
	})

	clear()
	t.Log("50,000 measurement rows into Postgres 16 (compose, synchronous_commit=on):")
	var insertLoop, copyBatched time.Duration
	for _, r := range results {
		perSecond := float64(r.rows) / r.elapsed.Seconds()
		t.Logf("  %-48s %8s  %9.0f rows/s", r.name, r.elapsed.Round(time.Millisecond), perSecond)
		switch {
		case r.name[:3] == "row":
			insertLoop = time.Duration(float64(r.elapsed) * float64(n) / float64(r.rows))
			t.Logf("  %-48s %8s  (extrapolated to %d rows)", "", insertLoop.Round(time.Millisecond), n)
		case r.name[:15] == "CopyFrom, 10K b":
			copyBatched = r.elapsed
		}
	}
	if copyBatched >= insertLoop {
		t.Fatalf("batched CopyFrom (%v) was not faster than the extrapolated INSERT loop (%v)",
			copyBatched, insertLoop)
	}
	_ = store.Config{}
	_ = model.Result{}
}
