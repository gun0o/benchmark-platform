package httpapi

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

func (s *Server) handleHealthz(w http.ResponseWriter, _ *http.Request) {
	// Liveness only: it must not touch Postgres, or a database blip would get the process
	// restarted instead of marked unready.
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) handleReadyz(w http.ResponseWriter, r *http.Request) {
	body := map[string]string{"status": "ok", "postgres": "ok", "redis": "disabled"}
	if err := s.store.Ping(r.Context()); err != nil {
		body["status"] = "unready"
		body["postgres"] = err.Error()
		writeJSON(w, http.StatusServiceUnavailable, body)
		return
	}
	// Redis is an optimization, so its absence is reported and is not an outage: the
	// endpoints still answer, from Postgres, and /readyz stays 200.
	if s.cache.Enabled() {
		body["redis"] = "ok"
		if err := s.cache.Ping(r.Context()); err != nil {
			body["redis"] = "degraded"
		}
	}
	writeJSON(w, http.StatusOK, body)
}

func (s *Server) handleIngestRun(w http.ResponseWriter, r *http.Request) {
	// The body is decoded as it is read and the results go to Postgres in batches, so a
	// 50,000-result envelope never exists in memory as one object graph.
	res, err := s.store.IngestStream(r.Context(), r.Body, s.maxRes)
	if err != nil {
		s.writeIngestError(w, err)
		return
	}
	// New rows for this machine make every cached answer about it stale, and nothing
	// else: machine A's ingest must not evict machine B's aggregates.
	if evicted := s.cache.InvalidateMachine(r.Context(), res.MachineID); evicted > 0 {
		s.log.Info("cache invalidated", "machine_id", res.MachineID, "keys", evicted)
	}
	s.log.Info("ingested run", "run_id", res.RunID, "machine_id", res.MachineID,
		"results", res.Results, "inserted", res.Inserted, "skipped", res.Skipped,
		"duplicate", res.Duplicate, "request_id", RequestID(r.Context()))
	writeJSON(w, http.StatusOK, res)
}

// writeIngestError maps the three kinds of ingest failure onto status codes: a document
// the API cannot parse, a document it parsed and rejected, and a database that failed.
func (s *Server) writeIngestError(w http.ResponseWriter, err error) {
	var tooMany *model.TooManyResultsError
	if errors.As(err, &tooMany) {
		writeError(w, http.StatusRequestEntityTooLarge, "too_many_results", tooMany.Error())
		return
	}
	var ve *model.ValidationError
	if errors.As(err, &ve) {
		writeError(w, http.StatusBadRequest, "schema_violation",
			"run envelope does not satisfy schema/benchmark-result.schema.json", ve.Problems...)
		return
	}
	// A JSON syntax error, an unknown field, or a wrongly typed value: all of them are the
	// client's document, not the server's state.
	if isDecodeError(err) {
		writeError(w, http.StatusBadRequest, "invalid_json",
			"request body is not a valid run envelope", err.Error())
		return
	}
	s.log.Error("ingest failed", "err", err)
	writeError(w, http.StatusInternalServerError, "ingest_failed", "could not store the run")
}

func isDecodeError(err error) bool {
	var syn *json.SyntaxError
	var typ *json.UnmarshalTypeError
	if errors.As(err, &syn) || errors.As(err, &typ) || errors.Is(err, io.EOF) ||
		errors.Is(err, io.ErrUnexpectedEOF) {
		return true
	}
	// encoding/json reports an unknown field and a few other shape problems as a plain
	// error string; the decoder is the only thing in this path that produces them.
	msg := err.Error()
	return strings.Contains(msg, "unknown field") || strings.Contains(msg, "run envelope:") ||
		strings.Contains(msg, "cannot unmarshal") || strings.Contains(msg, "invalid character")
}

func (s *Server) handleListMeasurements(w http.ResponseWriter, r *http.Request) {
	f, err := parseMeasurementFilter(r)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid_query", err.Error())
		return
	}
	rows, err := s.store.ListMeasurements(r.Context(), f)
	if err != nil {
		s.log.Error("list measurements failed", "err", err)
		writeError(w, http.StatusInternalServerError, "query_failed", "could not read measurements")
		return
	}
	page := store.MeasurementPage{Measurements: rows}
	if len(rows) == f.Limit && len(rows) > 0 {
		// Keyset: the next page starts after the last id of this one.
		page.NextCursor = strconv.FormatInt(rows[len(rows)-1].ID, 10)
	}
	writeJSON(w, http.StatusOK, page)
}

// maxPageLimit caps ?limit. 10000 is the Trials page's requirement (M6.3).
const maxPageLimit = 10_000

func parseMeasurementFilter(r *http.Request) (store.MeasurementFilter, error) {
	q := r.URL.Query()
	f := store.MeasurementFilter{
		MachineID: q.Get("machine_id"),
		RunID:     q.Get("run_id"),
		Workload:  q.Get("workload"),
		Metric:    q.Get("metric"),
		Limit:     100,
	}
	if f.Workload != "" && !model.ValidWorkload(f.Workload) {
		return f, fmt.Errorf("unknown workload %q", f.Workload)
	}
	if f.Metric != "" && !model.ValidMetric(f.Metric) {
		return f, fmt.Errorf("unknown metric %q", f.Metric)
	}
	if v := q.Get("thread_count"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 {
			return f, fmt.Errorf("thread_count must be a positive integer, got %q", v)
		}
		f.ThreadCount = &n
	}
	if v := q.Get("working_set_bytes"); v != "" {
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil || n < 0 {
			return f, fmt.Errorf("working_set_bytes must be a non-negative integer, got %q", v)
		}
		f.WorkingSetBytes = &n
	}
	if v := q.Get("limit"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 || n > maxPageLimit {
			return f, fmt.Errorf("limit must be between 1 and %d, got %q", maxPageLimit, v)
		}
		f.Limit = n
	}
	if v := q.Get("cursor"); v != "" {
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil || n < 0 {
			return f, fmt.Errorf("cursor must be a non-negative integer, got %q", v)
		}
		f.Cursor = n
	}
	return f, nil
}

func (s *Server) handleListMachines(w http.ResponseWriter, r *http.Request) {
	rows, err := s.store.ListMachines(r.Context())
	if err != nil {
		s.log.Error("list machines failed", "err", err)
		writeError(w, http.StatusInternalServerError, "query_failed", "could not read machines")
		return
	}
	if rows == nil {
		rows = []model.MachineRow{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"machines": rows})
}

func (s *Server) handleGetMachine(w http.ResponseWriter, r *http.Request) {
	m, err := s.store.GetMachine(r.Context(), r.PathValue("id"))
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, "not_found", "no such machine")
		return
	}
	if err != nil {
		s.log.Error("get machine failed", "err", err)
		writeError(w, http.StatusInternalServerError, "query_failed", "could not read the machine")
		return
	}
	writeJSON(w, http.StatusOK, m)
}

func (s *Server) handleListRuns(w http.ResponseWriter, r *http.Request) {
	limit := 100
	if v := r.URL.Query().Get("limit"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 || n > 1000 {
			writeError(w, http.StatusBadRequest, "invalid_query",
				fmt.Sprintf("limit must be between 1 and 1000, got %q", v))
			return
		}
		limit = n
	}
	rows, err := s.store.ListRuns(r.Context(), r.URL.Query().Get("machine_id"), limit)
	if err != nil {
		s.log.Error("list runs failed", "err", err)
		writeError(w, http.StatusInternalServerError, "query_failed", "could not read runs")
		return
	}
	if rows == nil {
		rows = []model.Run{}
	}
	writeJSON(w, http.StatusOK, map[string]any{"runs": rows})
}

func (s *Server) handleGetRun(w http.ResponseWriter, r *http.Request) {
	run, err := s.store.GetRun(r.Context(), r.PathValue("id"))
	if errors.Is(err, store.ErrNotFound) {
		writeError(w, http.StatusNotFound, "not_found", "no such run")
		return
	}
	if err != nil {
		s.log.Error("get run failed", "err", err)
		writeError(w, http.StatusInternalServerError, "query_failed", "could not read the run")
		return
	}
	writeJSON(w, http.StatusOK, run)
}
