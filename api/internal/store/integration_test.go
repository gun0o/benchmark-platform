//go:build integration

// Integration tests run against the compose Postgres (deploy/docker-compose.yml).
// Each test gets its own schema so they cannot see each other's rows:
//
//	go test -tags integration ./...
package store_test

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
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

// --- M5.1 -----------------------------------------------------------------------------

// envelopeJSON re-serializes a fixture so it can go through the streaming path, which is
// what the HTTP handler uses.
func envelopeJSON(t *testing.T, env *model.RunEnvelope) []byte {
	t.Helper()
	b, err := json.Marshal(env)
	if err != nil {
		t.Fatalf("marshal envelope: %v", err)
	}
	return b
}

func TestStreamIngestMatchesTheDecodedPath(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	env := syntheticEnvelope("22222222-3333-4444-8555-666666666666", 500)

	res, err := s.IngestStream(ctx, bytes.NewReader(envelopeJSON(t, env)), 50_000)
	if err != nil {
		t.Fatalf("ingest stream: %v", err)
	}
	if res.Inserted != 500 || res.Results != 500 || res.Skipped != 0 || res.Duplicate {
		t.Fatalf("%+v, want 500 inserted", res)
	}
	n, err := s.CountMeasurements(ctx, store.MeasurementFilter{})
	if err != nil {
		t.Fatalf("count: %v", err)
	}
	if n != 500 {
		t.Fatalf("count %d, want 500", n)
	}
}

// A run larger than one POST arrives as several envelopes sharing a run_id. They must
// append, not collide, and a chunk sent twice must add nothing.
func TestChunkedIngestAppendsAndRepeatsAreNoOps(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	full := syntheticEnvelope("33333333-4444-4555-8666-777777777777", 300)

	chunk := func(from, to int) []byte {
		part := *full
		part.Results = full.Results[from:to]
		return envelopeJSON(t, &part)
	}

	first, err := s.IngestStream(ctx, bytes.NewReader(chunk(0, 100)), 50_000)
	if err != nil {
		t.Fatalf("chunk 1: %v", err)
	}
	if first.Inserted != 100 || first.Duplicate {
		t.Fatalf("chunk 1: %+v", first)
	}
	second, err := s.IngestStream(ctx, bytes.NewReader(chunk(100, 300)), 50_000)
	if err != nil {
		t.Fatalf("chunk 2: %v", err)
	}
	if second.Inserted != 200 || second.Skipped != 0 || second.Duplicate {
		t.Fatalf("chunk 2: %+v, want 200 appended", second)
	}
	// Re-sending the first chunk (the client did not see the reply) adds nothing.
	repeat, err := s.IngestStream(ctx, bytes.NewReader(chunk(0, 100)), 50_000)
	if err != nil {
		t.Fatalf("repeat: %v", err)
	}
	if repeat.Inserted != 0 || repeat.Skipped != 100 || !repeat.Duplicate {
		t.Fatalf("repeat: %+v, want 0 inserted / 100 skipped / duplicate", repeat)
	}

	n, err := s.CountMeasurements(ctx, store.MeasurementFilter{})
	if err != nil {
		t.Fatalf("count: %v", err)
	}
	if n != 300 {
		t.Fatalf("count %d after three POSTs of a 300-result run, want 300", n)
	}
}

// An envelope that repeats a configuration inside itself must not abort the transaction:
// the fast path hits the unique index, rolls back to its savepoint and re-runs the batch
// through the conflict-aware path.
func TestSelfDuplicatingEnvelopeIsStoredOnce(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	env := syntheticEnvelope("44444444-5555-4666-8777-888888888888", 20)
	env.Results = append(env.Results, env.Results[0], env.Results[1])

	res, err := s.IngestStream(ctx, bytes.NewReader(envelopeJSON(t, env)), 50_000)
	if err != nil {
		t.Fatalf("ingest: %v", err)
	}
	if res.Results != 22 || res.Inserted != 20 || res.Skipped != 2 {
		t.Fatalf("%+v, want 22 posted / 20 inserted / 2 skipped", res)
	}
	n, _ := s.CountMeasurements(ctx, store.MeasurementFilter{})
	if n != 20 {
		t.Fatalf("count %d, want 20", n)
	}
}

func TestStreamIngestRejectsOversizeAndBadDocuments(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	env := syntheticEnvelope("55555555-6666-4777-8888-999999999999", 10)

	_, err := s.IngestStream(ctx, bytes.NewReader(envelopeJSON(t, env)), 5)
	var tooMany *model.TooManyResultsError
	if !errors.As(err, &tooMany) {
		t.Fatalf("err %v, want TooManyResultsError", err)
	}

	bad := syntheticEnvelope("66666666-7777-4888-8999-aaaaaaaaaaaa", 4)
	bad.Results[2].Metric = "cpu_vibes"
	_, err = s.IngestStream(ctx, bytes.NewReader(envelopeJSON(t, bad)), 50_000)
	var ve *model.ValidationError
	if !errors.As(err, &ve) {
		t.Fatalf("err %v, want ValidationError", err)
	}
	// A rejected document leaves nothing behind, not even the rows that preceded the bad
	// one, because the whole ingest is one transaction.
	n, _ := s.CountMeasurements(ctx, store.MeasurementFilter{})
	if n != 0 {
		t.Fatalf("count %d after two rejected POSTs, want 0", n)
	}
	runs, _ := s.ListRuns(ctx, "", 10)
	if len(runs) != 0 {
		t.Fatalf("%d runs after two rejected POSTs, want 0", len(runs))
	}
}

// Target for M5.1: a 50,000-result envelope ingests in under 2 seconds.
func TestLargeEnvelopeIngestTime(t *testing.T) {
	ctx := context.Background()
	s := newStore(t)
	env := syntheticEnvelope("77777777-8888-4999-8aaa-bbbbbbbbbbbb", 50_000)
	raw := envelopeJSON(t, env)

	start := time.Now()
	res, err := s.IngestStream(ctx, bytes.NewReader(raw), 50_000)
	elapsed := time.Since(start)
	if err != nil {
		t.Fatalf("ingest: %v", err)
	}
	if res.Inserted != 50_000 {
		t.Fatalf("%+v, want 50000 inserted", res)
	}
	t.Logf("50,000 results (%.1f MB of JSON) ingested in %v (%.0f rows/s)",
		float64(len(raw))/1e6, elapsed.Round(time.Millisecond),
		50_000/elapsed.Seconds())
	if elapsed > 2*time.Second {
		t.Fatalf("ingest took %v, target is under 2 s", elapsed)
	}
}
