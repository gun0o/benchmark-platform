package httpapi

import (
	"compress/gzip"
	"context"
	"crypto/rand"
	"encoding/hex"
	"io"
	"log/slog"
	"net/http"
	"strings"
	"sync"
	"time"
)

type ctxKey int

const requestIDKey ctxKey = 1

// RequestID returns the id assigned to this request, for log lines inside a handler.
func RequestID(ctx context.Context) string {
	id, _ := ctx.Value(requestIDKey).(string)
	return id
}

// Middleware is the conventional shape: a handler decorator.
type Middleware func(http.Handler) http.Handler

// Chain applies middleware so that the first argument is the outermost layer, i.e. the
// order they are listed is the order a request passes through them.
func Chain(h http.Handler, mw ...Middleware) http.Handler {
	for i := len(mw) - 1; i >= 0; i-- {
		h = mw[i](h)
	}
	return h
}

// recorder captures the status and byte count for the log line, and remembers whether the
// handler ever wrote, so the panic recovery knows if a 500 can still be sent.
type recorder struct {
	http.ResponseWriter
	status  int
	written int64
}

func (r *recorder) WriteHeader(status int) {
	if r.status == 0 {
		r.status = status
		r.ResponseWriter.WriteHeader(status)
	}
}

func (r *recorder) Write(b []byte) (int, error) {
	if r.status == 0 {
		r.status = http.StatusOK
	}
	n, err := r.ResponseWriter.Write(b)
	r.written += int64(n)
	return n, err
}

func (r *recorder) Unwrap() http.ResponseWriter { return r.ResponseWriter }

// WithRequestID accepts a caller's X-Request-ID or mints one, and echoes it back. It is
// the thing that ties an API log line to the k6 iteration or curl that caused it.
func WithRequestID(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		id := r.Header.Get("X-Request-ID")
		if id == "" || len(id) > 64 {
			var buf [8]byte
			if _, err := rand.Read(buf[:]); err != nil {
				id = "0000000000000000"
			} else {
				id = hex.EncodeToString(buf[:])
			}
		}
		w.Header().Set("X-Request-ID", id)
		next.ServeHTTP(w, r.WithContext(context.WithValue(r.Context(), requestIDKey, id)))
	})
}

// WithLogging logs one line per request with its duration.
func WithLogging(log *slog.Logger) Middleware {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			start := time.Now()
			rec := &recorder{ResponseWriter: w}
			next.ServeHTTP(rec, r)
			status := rec.status
			if status == 0 {
				status = http.StatusOK
			}
			level := slog.LevelInfo
			if status >= 500 {
				level = slog.LevelError
			}
			log.LogAttrs(r.Context(), level, "request",
				slog.String("method", r.Method),
				slog.String("path", r.URL.Path),
				slog.String("query", r.URL.RawQuery),
				slog.Int("status", status),
				slog.Int64("bytes", rec.written),
				slog.Duration("duration", time.Since(start)),
				slog.String("request_id", RequestID(r.Context())))
		})
	}
}

// WithRecovery turns a panic into a 500 and a log line instead of a dropped connection
// and a dead process.
func WithRecovery(log *slog.Logger) Middleware {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			rec := &recorder{ResponseWriter: w}
			defer func() {
				if v := recover(); v != nil {
					log.Error("panic", "err", v, "path", r.URL.Path,
						"request_id", RequestID(r.Context()))
					if rec.status == 0 {
						writeError(rec, http.StatusInternalServerError, "internal",
							"the request could not be completed")
					}
				}
			}()
			next.ServeHTTP(rec, r)
		})
	}
}

// WithTimeout gives every handler a context deadline. A query that outlives it is
// cancelled in Postgres too, because pgx passes the context down.
func WithTimeout(d time.Duration) Middleware {
	return func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			ctx, cancel := context.WithTimeout(r.Context(), d)
			defer cancel()
			next.ServeHTTP(w, r.WithContext(ctx))
		})
	}
}

var gzipPool = sync.Pool{
	New: func() any {
		gz, _ := gzip.NewWriterLevel(io.Discard, gzip.BestSpeed)
		return gz
	},
}

type gzipWriter struct {
	http.ResponseWriter
	gz     *gzip.Writer
	on     bool
	header bool
}

func (g *gzipWriter) WriteHeader(status int) {
	if !g.header {
		g.header = true
		// Only compress bodies worth compressing. A small JSON error envelope costs more
		// in CPU and header bytes than it saves.
		if cl := g.Header().Get("Content-Length"); cl == "" || len(cl) > 3 {
			g.Header().Set("Content-Encoding", "gzip")
			g.Header().Del("Content-Length")
			g.on = true
		}
	}
	g.ResponseWriter.WriteHeader(status)
}

func (g *gzipWriter) Write(b []byte) (int, error) {
	if !g.header {
		g.WriteHeader(http.StatusOK)
	}
	if !g.on {
		return g.ResponseWriter.Write(b)
	}
	return g.gz.Write(b)
}

// WithGzip compresses responses for clients that asked. Measurement pages are long runs
// of similar JSON; the dashboard's 10K-point Trials request is the case that matters.
func WithGzip(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
			next.ServeHTTP(w, r)
			return
		}
		gz := gzipPool.Get().(*gzip.Writer)
		gz.Reset(w)
		gw := &gzipWriter{ResponseWriter: w, gz: gz}
		defer func() {
			if gw.on {
				_ = gz.Close()
			}
			gzipPool.Put(gz)
		}()
		w.Header().Add("Vary", "Accept-Encoding")
		next.ServeHTTP(gw, r)
	})
}
