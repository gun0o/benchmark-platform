// Package httpapi is the HTTP layer: routing, handlers, middleware, error envelope.
// It is in internal/http/ per the repo layout; the package is named httpapi so that
// handlers can still refer to net/http as `http`.
package httpapi

import (
	"context"
	"log/slog"
	"net/http"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

// Store is what the handlers need from the repository. It is an interface so the handler
// tests can run against a fake with no database.
type Store interface {
	IngestRun(ctx context.Context, env *model.RunEnvelope) (store.IngestResult, error)
	ListMeasurements(ctx context.Context, f store.MeasurementFilter) ([]model.Measurement, error)
	Ping(ctx context.Context) error
}

// Server holds the handler dependencies.
type Server struct {
	store  Store
	log    *slog.Logger
	maxRes int // largest number of results one POST /v1/runs may carry
}

// Options configure a Server.
type Options struct {
	Log        *slog.Logger
	MaxResults int
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
	return &Server{store: s, log: log, maxRes: maxRes}
}

// Routes returns the mux. Go 1.22 method patterns mean the method is part of the pattern,
// so a GET to an ingest-only path is a 405 from the mux itself.
func (s *Server) Routes() *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", s.handleHealthz)
	mux.HandleFunc("GET /readyz", s.handleReadyz)
	mux.HandleFunc("POST /v1/runs", s.handleIngestRun)
	mux.HandleFunc("GET /v1/measurements", s.handleListMeasurements)
	return mux
}
