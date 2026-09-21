//go:build integration

package store_test

import (
	"context"
	"testing"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/seed"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

// TestSeededFilterCountsMatchSQL ingests generated data through the real path and then
// checks that every filter the list endpoint offers returns what an independent SQL count
// says it should. It is the M5.2 verification that the seeder and the query layer agree.
func TestSeededFilterCountsMatchSQL(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)

	o := seed.Options{Machines: 2, Trials: 5, Seed: 42, RunsPerMachine: 3,
		Now: time.Date(2026, 9, 21, 12, 0, 0, 0, time.UTC)}
	envs := seed.Generate(o)
	total := 0
	for _, env := range envs {
		res, err := s.IngestRun(ctx, env)
		if err != nil {
			t.Fatalf("ingest %s: %v", env.RunID, err)
		}
		total += res.Inserted
	}
	if total != o.Rows() {
		t.Fatalf("ingested %d rows, the generator promised %d", total, o.Rows())
	}

	machineID := seed.MachineID(seed.Profiles()[1])
	four, one := 4, 1
	ws := int64(64 << 20)
	filters := []struct {
		name string
		f    store.MeasurementFilter
		sql  string
		args []any
	}{
		{"everything", store.MeasurementFilter{}, `SELECT count(*) FROM measurements`, nil},
		{"machine", store.MeasurementFilter{MachineID: machineID},
			`SELECT count(*) FROM measurements WHERE machine_id = $1`, []any{machineID}},
		{"metric", store.MeasurementFilter{Metric: "mem_latency"},
			`SELECT count(*) FROM measurements WHERE metric = $1`, []any{"mem_latency"}},
		{"workload", store.MeasurementFilter{Workload: "disk_rand"},
			`SELECT count(*) FROM measurements WHERE workload = $1`, []any{"disk_rand"}},
		{"threads", store.MeasurementFilter{ThreadCount: &four},
			`SELECT count(*) FROM measurements WHERE thread_count = 4`, nil},
		{"working set", store.MeasurementFilter{WorkingSetBytes: &ws},
			`SELECT count(*) FROM measurements WHERE working_set_bytes = $1`, []any{ws}},
		{"all five", store.MeasurementFilter{MachineID: machineID, Metric: "mem_read_bw",
			Workload: "mem_bw", ThreadCount: &one, WorkingSetBytes: &ws},
			`SELECT count(*) FROM measurements WHERE machine_id = $1 AND metric = 'mem_read_bw'
			   AND workload = 'mem_bw' AND thread_count = 1 AND working_set_bytes = $2`,
			[]any{machineID, ws}},
	}

	for _, tc := range filters {
		t.Run(tc.name, func(t *testing.T) {
			var want int64
			if err := s.Pool().QueryRow(ctx, tc.sql, tc.args...).Scan(&want); err != nil {
				t.Fatalf("sql: %v", err)
			}
			got, err := s.CountMeasurements(ctx, tc.f)
			if err != nil {
				t.Fatalf("count: %v", err)
			}
			if got != want {
				t.Fatalf("filter count %d, SQL says %d", got, want)
			}
			if want == 0 {
				t.Fatal("the filter matched nothing; the check would pass vacuously")
			}
			// And paging with that filter must return exactly those rows.
			paged, cursor := 0, int64(0)
			for {
				f := tc.f
				f.Limit = 250
				f.Cursor = cursor
				rows, err := s.ListMeasurements(ctx, f)
				if err != nil {
					t.Fatalf("list: %v", err)
				}
				if len(rows) == 0 {
					break
				}
				paged += len(rows)
				cursor = rows[len(rows)-1].ID
			}
			if int64(paged) != want {
				t.Fatalf("paged %d rows through the filter, SQL says %d", paged, want)
			}
		})
	}

	// The twelve metrics must all be present, per machine.
	rows, err := s.Pool().Query(ctx,
		`SELECT metric, count(DISTINCT machine_id) FROM measurements GROUP BY metric`)
	if err != nil {
		t.Fatalf("query: %v", err)
	}
	defer rows.Close()
	seen := 0
	for rows.Next() {
		var metric string
		var machines int
		if err := rows.Scan(&metric, &machines); err != nil {
			t.Fatal(err)
		}
		if machines != 2 {
			t.Fatalf("metric %s appears for %d machines, want 2", metric, machines)
		}
		seen++
	}
	if seen != 12 {
		t.Fatalf("%d distinct metrics stored, want 12", seen)
	}
}
