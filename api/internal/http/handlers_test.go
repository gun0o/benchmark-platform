package httpapi_test

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
	"time"

	httpapi "github.com/matthewlee/benchmark-platform/api/internal/http"
	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

// fakeStore records what the handlers asked for and answers from memory. The handler
// tests need no Postgres; the store's own SQL is covered by the integration tests.
type fakeStore struct {
	ingested   []*model.RunEnvelope
	seenRunIDs map[string]bool
	rows       []model.Measurement
	machines   []model.MachineRow
	runs       []model.Run
	lastFilter store.MeasurementFilter
	ingestErr  error
	listErr    error
	pingErr    error
}

func newFake() *fakeStore { return &fakeStore{seenRunIDs: map[string]bool{}} }

// IngestStream mirrors the real store closely enough for the handler contract: decode
// (strictly, streaming), then remember the run and report what a second POST would do.
func (f *fakeStore) IngestStream(_ context.Context, body io.Reader, maxResults int) (store.IngestResult, error) {
	var results []model.Result
	env, err := model.StreamRunEnvelope(body, maxResults,
		func(_ *model.RunEnvelope, _ int, r *model.Result) error {
			results = append(results, *r)
			return nil
		})
	if err != nil {
		return store.IngestResult{}, err
	}
	if f.ingestErr != nil {
		return store.IngestResult{}, f.ingestErr
	}
	env.Results = results
	res := store.IngestResult{RunID: env.RunID, MachineID: env.Machine.ID, Results: len(results)}
	if f.seenRunIDs[env.RunID] {
		res.Duplicate = true
		res.Skipped = len(results)
		return res, nil
	}
	f.seenRunIDs[env.RunID] = true
	f.ingested = append(f.ingested, env)
	res.Inserted = len(results)
	return res, nil
}

func (f *fakeStore) ListMachines(context.Context) ([]model.MachineRow, error) {
	return f.machines, f.listErr
}

func (f *fakeStore) GetMachine(_ context.Context, id string) (model.MachineRow, error) {
	for _, m := range f.machines {
		if m.ID == id {
			return m, nil
		}
	}
	return model.MachineRow{}, store.ErrNotFound
}

func (f *fakeStore) ListRuns(_ context.Context, machineID string, limit int) ([]model.Run, error) {
	var out []model.Run
	for _, r := range f.runs {
		if machineID == "" || r.MachineID == machineID {
			out = append(out, r)
		}
	}
	if limit > 0 && len(out) > limit {
		out = out[:limit]
	}
	return out, f.listErr
}

func (f *fakeStore) GetRun(_ context.Context, id string) (model.Run, error) {
	for _, r := range f.runs {
		if r.ID == id {
			return r, nil
		}
	}
	return model.Run{}, store.ErrNotFound
}

func (f *fakeStore) ListMeasurements(_ context.Context, filter store.MeasurementFilter) ([]model.Measurement, error) {
	f.lastFilter = filter
	if f.listErr != nil {
		return nil, f.listErr
	}
	out := f.rows
	if filter.Limit > 0 && len(out) > filter.Limit {
		out = out[:filter.Limit]
	}
	return out, nil
}

func (f *fakeStore) Ping(context.Context) error { return f.pingErr }

func newServer(t *testing.T, f *fakeStore) http.Handler {
	t.Helper()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	// The full stack, not just the mux: the middleware is part of the contract these
	// tests assert (request ids, gzip, a panic that becomes a 500).
	return httpapi.NewServer(f, httpapi.Options{Log: log}).Handler()
}

func exampleEnvelope(t *testing.T) []byte {
	t.Helper()
	b, err := os.ReadFile("../../../schema/examples/run.json")
	if err != nil {
		t.Fatalf("read example: %v", err)
	}
	return b
}

func post(t *testing.T, h http.Handler, path, body string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

func get(t *testing.T, h http.Handler, path string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(http.MethodGet, path, nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

func TestIngestAcceptsExampleAndIsIdempotent(t *testing.T) {
	f := newFake()
	h := newServer(t, f)
	body := string(exampleEnvelope(t))

	rec := post(t, h, "/v1/runs", body)
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d: %s", rec.Code, rec.Body.String())
	}
	var res store.IngestResult
	if err := json.Unmarshal(rec.Body.Bytes(), &res); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if res.Inserted != 3 || res.Duplicate || res.Results != 3 {
		t.Fatalf("first POST: %+v, want inserted 3", res)
	}

	rec = post(t, h, "/v1/runs", body)
	if err := json.Unmarshal(rec.Body.Bytes(), &res); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if res.Inserted != 0 || !res.Duplicate {
		t.Fatalf("second POST: %+v, want inserted 0 and duplicate", res)
	}
}

func TestIngestRejections(t *testing.T) {
	base := string(exampleEnvelope(t))
	tests := []struct {
		name     string
		body     string
		status   int
		code     string
		contains string
	}{
		{"not json", "{", http.StatusBadRequest, "invalid_json", ""},
		{"empty body", "", http.StatusBadRequest, "invalid_json", ""},
		{
			"unknown metric",
			strings.Replace(base, `"metric": "cpu_int_ops"`, `"metric": "cpu_vibes"`, 1),
			http.StatusBadRequest, "schema_violation", "unknown metric",
		},
		{
			"wrong unit for metric",
			strings.Replace(base, `"unit": "ops/s"`, `"unit": "GB/s"`, 1),
			http.StatusBadRequest, "schema_violation", "measured in ops/s",
		},
		{
			"unknown field",
			strings.Replace(base, `"schema_version": 1,`, `"schema_version": 1, "oops": true,`, 1),
			http.StatusBadRequest, "invalid_json", "oops",
		},
		{
			"bad run_id",
			strings.Replace(base, `"5b1f5e0a-2a1e-4c8b-9a1e-9d6a9a1b2c3d"`, `"nope"`, 1),
			http.StatusBadRequest, "schema_violation", "run_id",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			h := newServer(t, newFake())
			rec := post(t, h, "/v1/runs", tc.body)
			if rec.Code != tc.status {
				t.Fatalf("status %d, want %d (%s)", rec.Code, tc.status, rec.Body.String())
			}
			var body struct {
				Error struct {
					Code    string   `json:"code"`
					Message string   `json:"message"`
					Details []string `json:"details"`
				} `json:"error"`
			}
			if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
				t.Fatalf("error envelope is not JSON: %v", err)
			}
			if body.Error.Code != tc.code {
				t.Fatalf("code %q, want %q", body.Error.Code, tc.code)
			}
			if tc.contains != "" && !strings.Contains(strings.Join(body.Error.Details, " "), tc.contains) {
				t.Fatalf("details %v do not mention %q", body.Error.Details, tc.contains)
			}
		})
	}
}

func TestIngestRefusesOversizeEnvelope(t *testing.T) {
	f := newFake()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	h := httpapi.NewServer(f, httpapi.Options{Log: log, MaxResults: 2}).Handler()
	rec := post(t, h, "/v1/runs", string(exampleEnvelope(t)))
	if rec.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("status %d, want 413: %s", rec.Code, rec.Body.String())
	}
}

func TestIngestStoreFailureIs500(t *testing.T) {
	f := newFake()
	f.ingestErr = fmt.Errorf("postgres is on fire")
	h := newServer(t, f)
	rec := post(t, h, "/v1/runs", string(exampleEnvelope(t)))
	if rec.Code != http.StatusInternalServerError {
		t.Fatalf("status %d, want 500", rec.Code)
	}
	if strings.Contains(rec.Body.String(), "on fire") {
		t.Fatal("internal error text leaked to the client")
	}
}

func TestMeasurementFilterParsing(t *testing.T) {
	ws := int64(0)
	tc7 := 7
	tests := []struct {
		query  string
		status int
		want   store.MeasurementFilter
	}{
		{query: "", status: 200, want: store.MeasurementFilter{Limit: 100}},
		{
			query:  "?machine_id=abc&metric=cpu_int_ops&workload=cpu_int&thread_count=7&working_set_bytes=0&limit=5&cursor=42",
			status: 200,
			want: store.MeasurementFilter{
				MachineID: "abc", Metric: "cpu_int_ops", Workload: "cpu_int",
				ThreadCount: &tc7, WorkingSetBytes: &ws, Limit: 5, Cursor: 42,
			},
		},
		{query: "?metric=nope", status: 400},
		{query: "?workload=nope", status: 400},
		{query: "?thread_count=0", status: 400},
		{query: "?limit=0", status: 400},
		{query: "?limit=10001", status: 400},
		{query: "?working_set_bytes=-1", status: 400},
	}
	for _, tc := range tests {
		t.Run(tc.query, func(t *testing.T) {
			f := newFake()
			h := newServer(t, f)
			rec := get(t, h, "/v1/measurements"+tc.query)
			if rec.Code != tc.status {
				t.Fatalf("status %d, want %d: %s", rec.Code, tc.status, rec.Body.String())
			}
			if tc.status != 200 {
				return
			}
			got := f.lastFilter
			if got.MachineID != tc.want.MachineID || got.Metric != tc.want.Metric ||
				got.Workload != tc.want.Workload || got.Limit != tc.want.Limit ||
				got.Cursor != tc.want.Cursor {
				t.Fatalf("filter %+v, want %+v", got, tc.want)
			}
			if (got.ThreadCount == nil) != (tc.want.ThreadCount == nil) ||
				(got.ThreadCount != nil && *got.ThreadCount != *tc.want.ThreadCount) {
				t.Fatalf("thread_count %v, want %v", got.ThreadCount, tc.want.ThreadCount)
			}
			if (got.WorkingSetBytes == nil) != (tc.want.WorkingSetBytes == nil) ||
				(got.WorkingSetBytes != nil && *got.WorkingSetBytes != *tc.want.WorkingSetBytes) {
				t.Fatalf("working_set_bytes %v, want %v", got.WorkingSetBytes, tc.want.WorkingSetBytes)
			}
		})
	}
}

func TestListReturnsCursorOnlyWhenPageIsFull(t *testing.T) {
	f := newFake()
	for i := 1; i <= 3; i++ {
		f.rows = append(f.rows, model.Measurement{ID: int64(i * 10), RecordedAt: time.Now()})
	}
	h := newServer(t, f)

	rec := get(t, h, "/v1/measurements?limit=3")
	var page store.MeasurementPage
	if err := json.Unmarshal(rec.Body.Bytes(), &page); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if page.NextCursor != "30" {
		t.Fatalf("next_cursor %q, want 30", page.NextCursor)
	}

	rec = get(t, h, "/v1/measurements?limit=10")
	page = store.MeasurementPage{}
	if err := json.Unmarshal(rec.Body.Bytes(), &page); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if page.NextCursor != "" {
		t.Fatalf("next_cursor %q on a partial page, want empty", page.NextCursor)
	}
}

func TestHealthzAndReadyz(t *testing.T) {
	f := newFake()
	h := newServer(t, f)
	if rec := get(t, h, "/healthz"); rec.Code != http.StatusOK {
		t.Fatalf("healthz %d", rec.Code)
	}
	if rec := get(t, h, "/readyz"); rec.Code != http.StatusOK {
		t.Fatalf("readyz %d", rec.Code)
	}
	f.pingErr = fmt.Errorf("connection refused")
	if rec := get(t, h, "/readyz"); rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("readyz with pg down: %d, want 503", rec.Code)
	}
	// Liveness must not depend on Postgres.
	if rec := get(t, h, "/healthz"); rec.Code != http.StatusOK {
		t.Fatalf("healthz with pg down: %d, want 200", rec.Code)
	}
}

func TestMethodNotAllowed(t *testing.T) {
	h := newServer(t, newFake())
	// /v1/runs answers GET (list) and POST (ingest); anything else is the mux's 405,
	// which comes from the method patterns rather than from handler code.
	for _, method := range []string{http.MethodDelete, http.MethodPut} {
		req := httptest.NewRequest(method, "/v1/runs", nil)
		rec := httptest.NewRecorder()
		h.ServeHTTP(rec, req)
		if rec.Code != http.StatusMethodNotAllowed {
			t.Fatalf("%s /v1/runs: %d, want 405", method, rec.Code)
		}
	}
	req := httptest.NewRequest(http.MethodPost, "/v1/measurements", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST /v1/measurements: %d, want 405", rec.Code)
	}
}
