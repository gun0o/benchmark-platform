package httpapi

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strconv"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

func (s *Server) handleHealthz(w http.ResponseWriter, _ *http.Request) {
	// Liveness only: it must not touch Postgres, or a database blip would get the process
	// restarted instead of marked unready.
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func (s *Server) handleReadyz(w http.ResponseWriter, r *http.Request) {
	body := map[string]string{"status": "ok", "postgres": "ok"}
	if err := s.store.Ping(r.Context()); err != nil {
		body["status"] = "unready"
		body["postgres"] = err.Error()
		writeJSON(w, http.StatusServiceUnavailable, body)
		return
	}
	writeJSON(w, http.StatusOK, body)
}

func (s *Server) handleIngestRun(w http.ResponseWriter, r *http.Request) {
	var env model.RunEnvelope
	dec := json.NewDecoder(r.Body)
	dec.DisallowUnknownFields() // strict decoding is half of the schema check
	if err := dec.Decode(&env); err != nil {
		writeError(w, http.StatusBadRequest, "invalid_json", "request body is not a valid run envelope", err.Error())
		return
	}
	if len(env.Results) > s.maxRes {
		writeError(w, http.StatusRequestEntityTooLarge, "too_many_results",
			fmt.Sprintf("a run envelope may carry at most %d results, got %d; chunk the run",
				s.maxRes, len(env.Results)))
		return
	}
	if err := env.Validate(); err != nil {
		var ve *model.ValidationError
		if errors.As(err, &ve) {
			writeError(w, http.StatusBadRequest, "schema_violation",
				"run envelope does not satisfy schema/benchmark-result.schema.json", ve.Problems...)
			return
		}
		writeError(w, http.StatusBadRequest, "schema_violation", err.Error())
		return
	}

	res, err := s.store.IngestRun(r.Context(), &env)
	if err != nil {
		s.log.Error("ingest failed", "run_id", env.RunID, "err", err)
		writeError(w, http.StatusInternalServerError, "ingest_failed", "could not store the run")
		return
	}
	s.log.Info("ingested run", "run_id", res.RunID, "machine_id", res.MachineID,
		"inserted", res.Inserted, "duplicate", res.Duplicate)
	writeJSON(w, http.StatusOK, res)
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
