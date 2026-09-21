//go:build integration

// Integration tests run against the compose Postgres (deploy/docker-compose.yml).
// Each test gets its own schema so they cannot see each other's rows:
//
//	go test -tags integration ./...
package store_test

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"
	"testing"
	"time"

	"github.com/jackc/pgx/v5"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
	"github.com/matthewlee/benchmark-platform/api/migrations"
)

func dsn() string {
	if v := os.Getenv("DATABASE_URL"); v != "" {
		return v
	}
	return "postgres://bench:bench@localhost:5432/bench?sslmode=disable"
}

// newStore gives the test an empty, migrated schema of its own.
func newStore(t *testing.T) *store.Store {
	t.Helper()
	ctx := context.Background()
	schema := "test_" + strings.ToLower(strings.NewReplacer("/", "_", "-", "_").Replace(t.Name()))

	admin, err := pgx.Connect(ctx, dsn())
	if err != nil {
		t.Skipf("no Postgres at %s: %v", dsn(), err)
	}
	defer admin.Close(ctx)
	for _, sql := range []string{
		fmt.Sprintf(`DROP SCHEMA IF EXISTS %s CASCADE`, schema),
		fmt.Sprintf(`CREATE SCHEMA %s`, schema),
	} {
		if _, err := admin.Exec(ctx, sql); err != nil {
			t.Fatalf("%s: %v", sql, err)
		}
	}

	url := dsn() + "&search_path=" + schema
	conn, err := pgx.Connect(ctx, url)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	if _, err := migrations.Up(ctx, conn); err != nil {
		t.Fatalf("migrate: %v", err)
	}
	conn.Close(ctx)

	s, err := store.New(ctx, store.Config{DSN: url, MaxConns: 4})
	if err != nil {
		t.Fatalf("store.New: %v", err)
	}
	t.Cleanup(func() {
		s.Close()
		admin, err := pgx.Connect(context.Background(), dsn())
		if err != nil {
			return
		}
		defer admin.Close(context.Background())
		_, _ = admin.Exec(context.Background(), fmt.Sprintf(`DROP SCHEMA IF EXISTS %s CASCADE`, schema))
	})
	return s
}

func fixtureEnvelope(t *testing.T) *model.RunEnvelope {
	t.Helper()
	b, err := os.ReadFile("../../../schema/examples/run.json")
	if err != nil {
		t.Fatalf("read example: %v", err)
	}
	var env model.RunEnvelope
	if err := json.Unmarshal(b, &env); err != nil {
		t.Fatalf("unmarshal example: %v", err)
	}
	if err := env.Validate(); err != nil {
		t.Fatalf("fixture invalid: %v", err)
	}
	return &env
}

func TestIngestRoundTrip(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	env := fixtureEnvelope(t)

	res, err := s.IngestRun(ctx, env)
	if err != nil {
		t.Fatalf("ingest: %v", err)
	}
	if res.Inserted != len(env.Results) || res.Duplicate {
		t.Fatalf("ingest: %+v, want inserted %d", res, len(env.Results))
	}

	rows, err := s.ListMeasurements(ctx, store.MeasurementFilter{Limit: 100})
	if err != nil {
		t.Fatalf("list: %v", err)
	}
	if len(rows) != len(env.Results) {
		t.Fatalf("read back %d rows, ingested %d", len(rows), len(env.Results))
	}
	first := rows[0]
	want := env.Results[0]
	if first.Metric != want.Metric || first.Value != want.Value || first.Unit != want.Unit ||
		first.ThreadCount != want.ThreadCount || first.Trial != want.Trial ||
		first.DurationNs != want.DurationNs {
		t.Fatalf("row %+v does not match result %+v", first, want)
	}
	if first.MachineID != env.Machine.ID || first.RunID != env.RunID {
		t.Fatalf("row is not attached to its run/machine: %+v", first)
	}
	// The machine block was hoisted out of the results; ingest flattens it back in.
	ts, _ := model.ParseTimestamp(want.Timestamp)
	if !first.RecordedAt.UTC().Equal(ts) {
		t.Fatalf("recorded_at %v, want %v", first.RecordedAt.UTC(), ts)
	}
	var params map[string]any
	if err := json.Unmarshal(first.Params, &params); err != nil {
		t.Fatalf("params did not survive as jsonb: %v", err)
	}
	if params["cold"] != "clflush" {
		t.Fatalf("params %v lost its cold mode", params)
	}
}

func TestIngestIsIdempotentOnRunID(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	env := fixtureEnvelope(t)

	if _, err := s.IngestRun(ctx, env); err != nil {
		t.Fatalf("first ingest: %v", err)
	}
	res, err := s.IngestRun(ctx, env)
	if err != nil {
		t.Fatalf("second ingest: %v", err)
	}
	if res.Inserted != 0 || !res.Duplicate {
		t.Fatalf("second ingest: %+v, want inserted 0 duplicate true", res)
	}
	n, err := s.CountMeasurements(ctx, store.MeasurementFilter{})
	if err != nil {
		t.Fatalf("count: %v", err)
	}
	if n != int64(len(env.Results)) {
		t.Fatalf("count %d after a duplicate POST, want %d", n, len(env.Results))
	}
}

func TestMachineUpsertKeepsFirstSeen(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	env := fixtureEnvelope(t)
	if _, err := s.IngestRun(ctx, env); err != nil {
		t.Fatalf("ingest: %v", err)
	}
	first, err := s.GetMachine(ctx, env.Machine.ID)
	if err != nil {
		t.Fatalf("get machine: %v", err)
	}

	later := *env
	later.RunID = "5b1f5e0a-2a1e-4c8b-9a1e-000000000002"
	later.StartedAt = "2026-10-01T10:00:00.000000Z"
	later.FinishedAt = "2026-10-01T10:00:05.000000Z"
	later.Machine.Hostname = "renamed-host"
	if _, err := s.IngestRun(ctx, &later); err != nil {
		t.Fatalf("second ingest: %v", err)
	}

	after, err := s.GetMachine(ctx, env.Machine.ID)
	if err != nil {
		t.Fatalf("get machine: %v", err)
	}
	if !after.FirstSeen.Equal(first.FirstSeen) {
		t.Fatalf("first_seen moved: %v -> %v", first.FirstSeen, after.FirstSeen)
	}
	if !after.LastSeen.After(first.LastSeen) {
		t.Fatalf("last_seen did not advance: %v -> %v", first.LastSeen, after.LastSeen)
	}
	if after.Hostname != "renamed-host" {
		t.Fatalf("hostname %q, want the newer one", after.Hostname)
	}
}

func TestKeysetPaginationCoversEveryRowExactlyOnce(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	env := syntheticEnvelope("11111111-2222-4333-8444-555555555555", 250)
	if _, err := s.IngestRun(ctx, env); err != nil {
		t.Fatalf("ingest: %v", err)
	}

	seen := map[int64]int{}
	cursor := int64(0)
	pages := 0
	for {
		rows, err := s.ListMeasurements(ctx, store.MeasurementFilter{Limit: 37, Cursor: cursor})
		if err != nil {
			t.Fatalf("page: %v", err)
		}
		if len(rows) == 0 {
			break
		}
		pages++
		for _, r := range rows {
			seen[r.ID]++
		}
		cursor = rows[len(rows)-1].ID
	}
	if len(seen) != 250 {
		t.Fatalf("paged over %d distinct rows, want 250 (%d pages)", len(seen), pages)
	}
	for id, n := range seen {
		if n != 1 {
			t.Fatalf("row %d returned %d times", id, n)
		}
	}
}

func TestFiltersNarrowAsAdvertised(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	env := syntheticEnvelope("11111111-2222-4333-8444-666666666666", 120)
	if _, err := s.IngestRun(ctx, env); err != nil {
		t.Fatalf("ingest: %v", err)
	}
	four := 4
	for _, tc := range []struct {
		name   string
		filter store.MeasurementFilter
		want   int64
	}{
		{"all", store.MeasurementFilter{}, 120},
		{"metric", store.MeasurementFilter{Metric: "cpu_int_ops"}, 60},
		{"workload", store.MeasurementFilter{Workload: "cpu_fp"}, 60},
		{"thread_count", store.MeasurementFilter{ThreadCount: &four}, 60},
		{"metric+threads", store.MeasurementFilter{Metric: "cpu_int_ops", ThreadCount: &four}, 30},
		{"machine", store.MeasurementFilter{MachineID: env.Machine.ID}, 120},
		{"other machine", store.MeasurementFilter{MachineID: "0"}, 0},
	} {
		t.Run(tc.name, func(t *testing.T) {
			n, err := s.CountMeasurements(ctx, tc.filter)
			if err != nil {
				t.Fatalf("count: %v", err)
			}
			if n != tc.want {
				t.Fatalf("count %d, want %d", n, tc.want)
			}
		})
	}
}

// syntheticEnvelope builds n results alternating cpu_int/cpu_fp over 1 and 4 threads.
func syntheticEnvelope(runID string, n int) *model.RunEnvelope {
	env := &model.RunEnvelope{
		SchemaVersion: 1,
		RunID:         runID,
		StartedAt:     "2026-09-21T10:00:00.000000Z",
		FinishedAt:    "2026-09-21T10:01:00.000000Z",
		Machine: model.Machine{
			ID: "ffffffffffffffffffffffffffffffff", Hostname: "fixture", CPUModel: "fixture cpu",
			PhysicalCores: 4, LogicalCPUs: 8, L1dKB: 48, L2KB: 2048, L3KB: 24576,
			MemoryBytes: 1 << 34, OS: "linux", Kernel: "test", Compiler: "g++",
			CompilerFlags: "-O3", EngineVersion: "test", EngineGitSHA: "0000000",
		},
		Argv: []string{"bench", "run"},
	}
	base := time.Date(2026, 9, 21, 10, 0, 0, 0, time.UTC)
	for i := 0; i < n; i++ {
		metric, workload := "cpu_int_ops", "cpu_int"
		if i%2 == 1 {
			metric, workload = "cpu_fp_ops", "cpu_fp"
		}
		threads := 1
		if (i/2)%2 == 1 {
			threads = 4
		}
		env.Results = append(env.Results, model.Result{
			Workload: workload, Metric: metric, Unit: "ops/s", ThreadCount: threads,
			WorkingSetBytes: 0, Trial: i / 4, Value: float64(1e9 + i),
			Timestamp:  base.Add(time.Duration(i) * time.Millisecond).Format("2006-01-02T15:04:05.000000Z"),
			DurationNs: 50_000_000,
			Params:     json.RawMessage(`{"cold":"clflush","pinned":false}`),
		})
	}
	return env
}
