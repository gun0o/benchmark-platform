package httpapi_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

func fakeWithSeries() *fakeStore {
	f := fakeWithInventory()
	f.aggregates = store.AggregateResponse{
		MachineID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", Metric: "cpu_int_ops", Unit: "ops/s",
		GroupBy: store.GroupByThreadCount,
		Groups: []store.AggregateGroup{
			{Group: 1, N: 100, Mean: 4.6e9, Median: 4.6e9, Stddev: 4.6e7, CoV: 0.01},
			{Group: 2, N: 100, Mean: 9.0e9, Median: 9.0e9, Stddev: 9.0e7, CoV: 0.01},
		},
	}
	base := time.Date(2026, 9, 21, 10, 0, 0, 0, time.UTC)
	for i := 0; i < 5000; i++ {
		f.trials = append(f.trials, store.TrialPoint{
			Trial: i, Value: 4.6e9 + float64(i%17)*1e6, RecordedAt: base.Add(time.Duration(i) * time.Millisecond),
		})
	}
	return f
}

func TestAggregatesRequireMachineAndMetric(t *testing.T) {
	h := newServer(t, fakeWithSeries())
	for _, q := range []string{
		"",
		"?machine_id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		"?metric=cpu_int_ops",
	} {
		rec := get(t, h, "/v1/aggregates"+q)
		if rec.Code != http.StatusBadRequest {
			t.Fatalf("%q: status %d, want 400", q, rec.Code)
		}
	}
	rec := get(t, h, "/v1/aggregates?machine_id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&metric=cpu_int_ops")
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d: %s", rec.Code, rec.Body.String())
	}
	var res store.AggregateResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &res); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(res.Groups) != 2 || res.Groups[1].Group != 2 {
		t.Fatalf("groups %+v", res.Groups)
	}
}

func TestAggregatesRejectBadParameters(t *testing.T) {
	f := fakeWithSeries()
	h := newServer(t, f)
	const ok = "machine_id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&metric=cpu_int_ops"
	for _, q := range []string{
		"?" + ok + "&group_by=hostname",
		"?" + ok + "&workload=nope",
		"?machine_id=x&metric=nope",
		"?" + ok + "&working_set_bytes=-5",
	} {
		if rec := get(t, h, "/v1/aggregates"+q); rec.Code != http.StatusBadRequest {
			t.Fatalf("%q: status %d, want 400", q, rec.Code)
		}
	}
	// group_by=working_set_bytes pins thread_count instead, and passes it through.
	rec := get(t, h, "/v1/aggregates?"+ok+"&group_by=working_set_bytes&thread_count=8")
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d: %s", rec.Code, rec.Body.String())
	}
	if f.lastGroupBy != store.GroupByWorkingSetBytes {
		t.Fatalf("group_by %q", f.lastGroupBy)
	}
	if f.lastFilter.ThreadCount == nil || *f.lastFilter.ThreadCount != 8 {
		t.Fatalf("thread_count filter %v", f.lastFilter.ThreadCount)
	}
}

func TestCompareNeedsTwoMachines(t *testing.T) {
	f := fakeWithSeries()
	h := newServer(t, f)
	for _, q := range []string{
		"?metric=cpu_int_ops",
		"?metric=cpu_int_ops&machines=a",
		"?machines=a,b",
		"?metric=nope&machines=a,b",
		"?metric=cpu_int_ops&machines=a,b,c,d,e,f,g,h,i",
	} {
		if rec := get(t, h, "/v1/compare"+q); rec.Code != http.StatusBadRequest {
			t.Fatalf("%q: status %d, want 400", q, rec.Code)
		}
	}
	rec := get(t, h, "/v1/compare?metric=cpu_int_ops&machines=a,b")
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d: %s", rec.Code, rec.Body.String())
	}
	if len(f.lastMachines) != 2 || f.lastMachines[0] != "a" {
		t.Fatalf("machines %v; order matters, the first is the baseline", f.lastMachines)
	}
}

func TestTrialsCompactShapeAndDownsampling(t *testing.T) {
	f := fakeWithSeries()
	h := newServer(t, f)
	const base = "/v1/trials?machine_id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&metric=cpu_int_ops"

	rec := get(t, h, base+"&limit=5000")
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d: %s", rec.Code, rec.Body.String())
	}
	var res struct {
		N          int     `json:"n"`
		Unit       string  `json:"unit"`
		Downsample string  `json:"downsample"`
		Points     [][]any `json:"points"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &res); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if res.N != 5000 || len(res.Points) != 5000 {
		t.Fatalf("n=%d points=%d, want 5000", res.N, len(res.Points))
	}
	if len(res.Points[0]) != 3 {
		t.Fatalf("a point is %d values, want [trial, value, recorded_at]", len(res.Points[0]))
	}
	if res.Unit != "ops/s" {
		t.Fatalf("unit %q", res.Unit)
	}

	rec = get(t, h, base+"&limit=5000&downsample=lttb:500")
	if err := json.Unmarshal(rec.Body.Bytes(), &res); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if res.N != 500 || res.Downsample != "lttb:500" {
		t.Fatalf("downsampled n=%d downsample=%q", res.N, res.Downsample)
	}

	for _, q := range []string{"&downsample=nearest:100", "&downsample=lttb:2", "&limit=99999"} {
		if rec := get(t, h, base+q); rec.Code != http.StatusBadRequest {
			t.Fatalf("%q: status %d, want 400", q, rec.Code)
		}
	}
}

// Without Redis configured every request is a miss served from Postgres, and the header
// says so rather than pretending.
func TestCacheHeaderSaysMissWhenThereIsNoCache(t *testing.T) {
	h := newServer(t, fakeWithSeries())
	rec := get(t, h, "/v1/aggregates?machine_id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&metric=cpu_int_ops")
	if got := rec.Header().Get("X-Cache"); got != "miss" {
		t.Fatalf("X-Cache %q, want miss", got)
	}
}
