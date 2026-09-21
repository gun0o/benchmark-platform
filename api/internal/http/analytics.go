package httpapi

import (
	"fmt"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"

	"github.com/matthewlee/benchmark-platform/api/internal/cache"
	"github.com/matthewlee/benchmark-platform/api/internal/lttb"
	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

// cacheKey builds a canonical key from the query parameters that actually affect the
// answer: names sorted, values sorted within a name, endpoint prefixed. Two requests that
// mean the same thing must produce the same key, and two that do not must not - a key
// that forgot one filter would serve one query's answer to another.
func cacheKey(endpoint string, q url.Values, params ...string) string {
	names := append([]string(nil), params...)
	sort.Strings(names)
	var b strings.Builder
	b.WriteString(endpoint)
	for _, name := range names {
		values := q[name]
		if len(values) == 0 {
			continue
		}
		sorted := append([]string(nil), values...)
		sort.Strings(sorted)
		b.WriteByte('|')
		b.WriteString(name)
		b.WriteByte('=')
		b.WriteString(strings.Join(sorted, ","))
	}
	return b.String()
}

// writeCached sends a body that is already JSON, with a header saying where it came from.
func writeCached(w http.ResponseWriter, body []byte, hit bool) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	if hit {
		w.Header().Set("X-Cache", "hit")
	} else {
		w.Header().Set("X-Cache", "miss")
	}
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(body)
}

// requireFilters enforces what the aggregate endpoints will not do without: a machine and
// a metric. percentile_cont over the whole table is a query this API should not accept by
// accident.
func requireFilters(q url.Values, w http.ResponseWriter) (string, string, bool) {
	machineID := q.Get("machine_id")
	metric := q.Get("metric")
	if machineID == "" || metric == "" {
		writeError(w, http.StatusBadRequest, "missing_filter",
			"machine_id and metric are required; an aggregate over every machine and metric "+
				"is not a question this endpoint answers")
		return "", "", false
	}
	if !model.ValidMetric(metric) {
		writeError(w, http.StatusBadRequest, "invalid_query",
			fmt.Sprintf("unknown metric %q", metric))
		return "", "", false
	}
	return machineID, metric, true
}

func (s *Server) handleAggregates(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	machineID, metric, ok := requireFilters(q, w)
	if !ok {
		return
	}
	by, err := store.ParseGroupBy(q.Get("group_by"))
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid_query", err.Error())
		return
	}
	f := store.MeasurementFilter{MachineID: machineID, Metric: metric, Workload: q.Get("workload")}
	if f.Workload != "" && !model.ValidWorkload(f.Workload) {
		writeError(w, http.StatusBadRequest, "invalid_query",
			fmt.Sprintf("unknown workload %q", f.Workload))
		return
	}
	// When grouping by one dimension, the other may be pinned.
	if by == store.GroupByThreadCount {
		if v := q.Get("working_set_bytes"); v != "" {
			n, err := strconv.ParseInt(v, 10, 64)
			if err != nil || n < 0 {
				writeError(w, http.StatusBadRequest, "invalid_query",
					"working_set_bytes must be a non-negative integer")
				return
			}
			f.WorkingSetBytes = &n
		}
	} else if v := q.Get("thread_count"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 {
			writeError(w, http.StatusBadRequest, "invalid_query",
				"thread_count must be a positive integer")
			return
		}
		f.ThreadCount = &n
	}

	key := cacheKey("agg", q, "machine_id", "metric", "workload", "group_by",
		"thread_count", "working_set_bytes")
	body, hit, err := cache.Fetch(r.Context(), s.cache, key, []string{machineID}, func() (any, error) {
		return s.store.Aggregates(r.Context(), f, by)
	})
	if err != nil {
		s.log.Error("aggregates failed", "err", err, "request_id", RequestID(r.Context()))
		writeError(w, http.StatusInternalServerError, "query_failed", "could not aggregate")
		return
	}
	writeCached(w, body, hit)
}

func (s *Server) handleCompare(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	metric := q.Get("metric")
	if metric == "" || !model.ValidMetric(metric) {
		writeError(w, http.StatusBadRequest, "invalid_query",
			fmt.Sprintf("metric is required and must be one of the 12 metrics, got %q", metric))
		return
	}
	var machines []string
	for _, m := range strings.Split(q.Get("machines"), ",") {
		if m = strings.TrimSpace(m); m != "" {
			machines = append(machines, m)
		}
	}
	if len(machines) < 2 {
		writeError(w, http.StatusBadRequest, "invalid_query",
			"machines must list at least two machine ids, comma-separated")
		return
	}
	const maxCompare = 8
	if len(machines) > maxCompare {
		writeError(w, http.StatusBadRequest, "invalid_query",
			fmt.Sprintf("at most %d machines may be compared at once", maxCompare))
		return
	}
	by, err := store.ParseGroupBy(q.Get("group_by"))
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid_query", err.Error())
		return
	}
	f := store.MeasurementFilter{Metric: metric, Workload: q.Get("workload")}
	if by == store.GroupByThreadCount {
		if v := q.Get("working_set_bytes"); v != "" {
			n, err := strconv.ParseInt(v, 10, 64)
			if err != nil || n < 0 {
				writeError(w, http.StatusBadRequest, "invalid_query",
					"working_set_bytes must be a non-negative integer")
				return
			}
			f.WorkingSetBytes = &n
		}
	} else if v := q.Get("thread_count"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 {
			writeError(w, http.StatusBadRequest, "invalid_query",
				"thread_count must be a positive integer")
			return
		}
		f.ThreadCount = &n
	}

	// The key keeps the machines in the order given, because the first one is the
	// baseline every ratio is computed against: a,b and b,a are different answers.
	key := cacheKey("cmp", q, "metric", "workload", "group_by", "thread_count",
		"working_set_bytes") + "|machines=" + strings.Join(machines, ",")
	body, hit, err := cache.Fetch(r.Context(), s.cache, key, machines, func() (any, error) {
		return s.store.Compare(r.Context(), machines, f, by)
	})
	if err != nil {
		s.log.Error("compare failed", "err", err, "request_id", RequestID(r.Context()))
		writeError(w, http.StatusInternalServerError, "query_failed", "could not compare")
		return
	}
	writeCached(w, body, hit)
}

// maxTrials is the Trials page's requirement from M6.3.
const maxTrials = 10_000

func (s *Server) handleTrials(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	machineID, metric, ok := requireFilters(q, w)
	if !ok {
		return
	}
	f := store.MeasurementFilter{
		MachineID: machineID, Metric: metric, Workload: q.Get("workload"), Limit: maxTrials,
	}
	if v := q.Get("thread_count"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 {
			writeError(w, http.StatusBadRequest, "invalid_query",
				"thread_count must be a positive integer")
			return
		}
		f.ThreadCount = &n
	}
	if v := q.Get("working_set_bytes"); v != "" {
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil || n < 0 {
			writeError(w, http.StatusBadRequest, "invalid_query",
				"working_set_bytes must be a non-negative integer")
			return
		}
		f.WorkingSetBytes = &n
	}
	if v := q.Get("limit"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 || n > maxTrials {
			writeError(w, http.StatusBadRequest, "invalid_query",
				fmt.Sprintf("limit must be between 1 and %d", maxTrials))
			return
		}
		f.Limit = n
	}
	downsample := q.Get("downsample")
	threshold, err := parseDownsample(downsample)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid_query", err.Error())
		return
	}

	key := cacheKey("trials", q, "machine_id", "metric", "workload", "thread_count",
		"working_set_bytes", "limit", "downsample")
	body, hit, err := cache.Fetch(r.Context(), s.cache, key, []string{machineID}, func() (any, error) {
		points, err := s.store.Trials(r.Context(), f)
		if err != nil {
			return nil, err
		}
		return buildTrialsResponse(f, points, threshold, downsample), nil
	})
	if err != nil {
		s.log.Error("trials failed", "err", err, "request_id", RequestID(r.Context()))
		writeError(w, http.StatusInternalServerError, "query_failed", "could not read trials")
		return
	}
	writeCached(w, body, hit)
}

// parseDownsample accepts "lttb:N".
func parseDownsample(v string) (int, error) {
	if v == "" {
		return 0, nil
	}
	name, count, found := strings.Cut(v, ":")
	if !found || name != "lttb" {
		return 0, fmt.Errorf("downsample must be lttb:N, got %q", v)
	}
	n, err := strconv.Atoi(count)
	if err != nil || n < 3 || n > maxTrials {
		return 0, fmt.Errorf("downsample threshold must be between 3 and %d", maxTrials)
	}
	return n, nil
}

func buildTrialsResponse(f store.MeasurementFilter, points []store.TrialPoint, threshold int, downsample string) store.TrialsResponse {
	res := store.TrialsResponse{
		MachineID: f.MachineID, Metric: f.Metric, Unit: model.UnitOf[f.Metric],
		Points: make([][3]any, 0, len(points)),
	}
	keep := points
	if threshold > 0 && threshold < len(points) {
		in := make([]lttb.Point, len(points))
		for i, p := range points {
			in[i] = lttb.Point{X: float64(p.Trial), Y: p.Value}
		}
		out := lttb.Downsample(in, threshold)
		// Map the survivors back to their rows. LTTB keeps original points, so matching on
		// the x value is exact, and the series is in trial order on both sides.
		keep = keep[:0]
		j := 0
		for _, p := range out {
			for j < len(points) && float64(points[j].Trial) != p.X {
				j++
			}
			if j < len(points) {
				keep = append(keep, points[j])
				j++
			}
		}
		res.Downsample = downsample
	}
	for _, p := range keep {
		res.Points = append(res.Points, [3]any{p.Trial, p.Value, p.RecordedAt.Format("2006-01-02T15:04:05.000000Z")})
	}
	res.N = len(res.Points)
	return res
}
