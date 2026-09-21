// Package httpapi is the HTTP layer: routing, handlers, middleware, error envelope.
// It is in internal/http/ per the repo layout; the package is named httpapi so that
// handlers can still refer to net/http as `http`.
package httpapi

import (
	"context"
	"io"
	"log/slog"
	"net/http"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

// Store is what the handlers need from the repository. It is an interface so the handler
// tests can run against a fake with no database.
type Store interface {
	IngestStream(ctx context.Context, body io.Reader, maxResults int) (store.IngestResult, error)
	ListMeasurements(ctx context.Context, f store.MeasurementFilter) ([]model.Measurement, error)
	ListMachines(ctx context.Context) ([]model.MachineRow, error)
	GetMachine(ctx context.Context, id string) (model.MachineRow, error)
	ListRuns(ctx context.Context, machineID string, limit int) ([]model.Run, error)
	GetRun(ctx context.Context, id string) (model.Run, error)
	Ping(ctx context.Context) error
}

// Server holds the handler dependencies.
type Server struct {
	store   Store
	log     *slog.Logger
	maxRes  int           // largest number of results one POST /v1/runs may carry
	timeout time.Duration // per-handler context deadline
}

// Options configure a Server.
type Options struct {
	Log            *slog.Logger
	MaxResults     int
	HandlerTimeout time.Duration
}

// DefaultMaxResults is the ingest size limit. Larger runs are chunked by the client; the
// engine's --post does that.
const DefaultMaxResults = 50_000

// NewServer builds the handlers.
func NewServer(s Store, opts Options) *Server {
	log := opts.Log
	if log == nil {
		log = slog.Default()
	}
	maxRes := opts.MaxResults
	if maxRes <= 0 {
		maxRes = DefaultMaxResults
	}
	timeout := opts.HandlerTimeout
	if timeout <= 0 {
		timeout = 5 * time.Second
	}
	return &Server{store: s, log: log, maxRes: maxRes, timeout: timeout}
}

// Routes returns the mux. Go 1.22 method patterns mean the method is part of the pattern,
// so a GET to an ingest-only path is a 405 from the mux itself.
func (s *Server) Routes() *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", s.handleHealthz)
	mux.HandleFunc("GET /readyz", s.handleReadyz)
	mux.HandleFunc("POST /v1/runs", s.handleIngestRun)
	mux.HandleFunc("GET /v1/measurements", s.handleListMeasurements)
	mux.HandleFunc("GET /v1/machines", s.handleListMachines)
	mux.HandleFunc("GET /v1/machines/{id}", s.handleGetMachine)
	mux.HandleFunc("GET /v1/runs", s.handleListRuns)
	mux.HandleFunc("GET /v1/runs/{id}", s.handleGetRun)
	return mux
}

// Handler is Routes wrapped in the middleware stack, outermost first: an id for every
// request, a log line for every request, a panic that becomes a 500, a deadline that
// reaches Postgres, and gzip for clients that asked.
//
// Ingest is deliberately outside the handler deadline set here: the timeout applies to
// reading and storing a body that may be tens of megabytes, and 5 seconds is a query
// budget, not an upload budget. It gets the server's WriteTimeout instead.
func (s *Server) Handler() http.Handler {
	mux := s.Routes()
	return Chain(mux,
		WithRequestID,
		WithLogging(s.log),
		WithRecovery(s.log),
		WithGzip,
		s.timeoutExceptIngest(),
	)
}

func (s *Server) timeoutExceptIngest() Middleware {
	timeout := WithTimeout(s.timeout)
	return func(next http.Handler) http.Handler {
		timed := timeout(next)
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.Method == http.MethodPost && r.URL.Path == "/v1/runs" {
				next.ServeHTTP(w, r)
				return
			}
			timed.ServeHTTP(w, r)
		})
	}
}
