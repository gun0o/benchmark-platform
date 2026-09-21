package httpapi_test

import (
	"compress/gzip"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	httpapi "github.com/matthewlee/benchmark-platform/api/internal/http"
	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

func TestRequestIDIsEchoedOrMinted(t *testing.T) {
	h := newServer(t, newFake())

	req := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	req.Header.Set("X-Request-ID", "from-the-client")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if got := rec.Header().Get("X-Request-ID"); got != "from-the-client" {
		t.Fatalf("X-Request-ID %q, want the caller's", got)
	}

	rec = get(t, h, "/healthz")
	if got := rec.Header().Get("X-Request-ID"); len(got) != 16 {
		t.Fatalf("minted X-Request-ID %q, want 16 hex chars", got)
	}
}

func TestGzipWhenAskedAndOnlyThen(t *testing.T) {
	f := newFake()
	for i := 1; i <= 200; i++ {
		f.rows = append(f.rows, model.Measurement{
			ID: int64(i), Metric: "cpu_int_ops", Unit: "ops/s", Workload: "cpu_int",
			Value: 1.234e9, RecordedAt: time.Now(),
		})
	}
	h := newServer(t, f)

	plain := get(t, h, "/v1/measurements?limit=200")
	if enc := plain.Header().Get("Content-Encoding"); enc != "" {
		t.Fatalf("Content-Encoding %q without Accept-Encoding", enc)
	}

	req := httptest.NewRequest(http.MethodGet, "/v1/measurements?limit=200", nil)
	req.Header.Set("Accept-Encoding", "gzip")
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if enc := rec.Header().Get("Content-Encoding"); enc != "gzip" {
		t.Fatalf("Content-Encoding %q, want gzip", enc)
	}
	if !strings.Contains(rec.Header().Get("Vary"), "Accept-Encoding") {
		t.Fatal("a compressible response must Vary: Accept-Encoding or caches will serve it wrong")
	}
	// Read the compressed length before decompressing: gzip.NewReader drains rec.Body.
	compressed := rec.Body.Len()
	zr, err := gzip.NewReader(rec.Body)
	if err != nil {
		t.Fatalf("response is not gzip: %v", err)
	}
	body, err := io.ReadAll(zr)
	if err != nil {
		t.Fatalf("gunzip: %v", err)
	}
	var page struct {
		Measurements []model.Measurement `json:"measurements"`
	}
	if err := json.Unmarshal(body, &page); err != nil {
		t.Fatalf("decompressed body is not the JSON page: %v", err)
	}
	if len(page.Measurements) != 200 {
		t.Fatalf("decompressed %d rows, want 200", len(page.Measurements))
	}
	if compressed >= len(body) {
		t.Fatalf("gzip made it bigger: %d compressed vs %d plain", compressed, len(body))
	}
	t.Logf("200 measurement rows: %d bytes plain, %d bytes gzip (%.1fx)",
		len(body), compressed, float64(len(body))/float64(compressed))
}

// A panicking handler must become a 500, not a dropped connection and a dead process.
func TestRecoveryTurnsAPanicIntoA500(t *testing.T) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	mux := http.NewServeMux()
	mux.HandleFunc("GET /boom", func(http.ResponseWriter, *http.Request) {
		panic("kaboom")
	})
	h := httpapi.Chain(mux, httpapi.WithRequestID, httpapi.WithLogging(log), httpapi.WithRecovery(log))

	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/boom", nil))
	if rec.Code != http.StatusInternalServerError {
		t.Fatalf("status %d, want 500", rec.Code)
	}
	if strings.Contains(rec.Body.String(), "kaboom") {
		t.Fatal("the panic value leaked to the client")
	}
}

func TestHandlerDeadlineReachesTheHandler(t *testing.T) {
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	var deadlineSet bool
	mux := http.NewServeMux()
	mux.HandleFunc("GET /slow", func(w http.ResponseWriter, r *http.Request) {
		_, deadlineSet = r.Context().Deadline()
		w.WriteHeader(http.StatusOK)
	})
	h := httpapi.Chain(mux, httpapi.WithTimeout(50*time.Millisecond))
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/slow", nil))
	if !deadlineSet {
		t.Fatal("handler ran without a context deadline")
	}
	_ = log
}
