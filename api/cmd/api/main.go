// api serves the benchmark REST API on API_ADDR (default :8080).
package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"runtime"
	"syscall"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/cache"
	"github.com/matthewlee/benchmark-platform/api/internal/config"
	httpapi "github.com/matthewlee/benchmark-platform/api/internal/http"
	"github.com/matthewlee/benchmark-platform/api/internal/store"
)

func main() {
	cfg := config.Load()
	log := newLogger(cfg.LogLevel)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	startCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	st, err := store.New(startCtx, store.Config{DSN: cfg.DatabaseURL, MaxConns: cfg.PGMaxConns})
	if err != nil {
		log.Error("cannot reach postgres", "err", err)
		os.Exit(1)
	}
	defer st.Close()

	rc, err := cache.New(cache.Options{URL: cfg.RedisURL, TTL: cfg.CacheTTL, Log: log})
	if err != nil {
		// A malformed REDIS_URL is a configuration error worth failing on; an unreachable
		// Redis is not, and is handled per request.
		log.Error("bad REDIS_URL", "err", err)
		os.Exit(1)
	}
	defer rc.Close()
	if rc.Enabled() {
		if err := rc.Ping(startCtx); err != nil {
			log.Warn("redis not reachable at startup; serving from postgres", "err", err)
		}
	}

	srv := httpapi.NewServer(st, httpapi.Options{
		Log:            log,
		MaxResults:     cfg.MaxResults,
		HandlerTimeout: cfg.HandlerLimit,
		Cache:          rc,
	})
	server := &http.Server{
		Addr:    cfg.Addr,
		Handler: srv.Handler(),
		// Go's zero-value http.Server has no limits at all: a client that opens a
		// connection and never finishes its headers holds a goroutine forever.
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       cfg.ReadTimeout,
		WriteTimeout:      cfg.WriteTimeout,
		IdleTimeout:       120 * time.Second,
		MaxHeaderBytes:    1 << 20,
		ErrorLog:          slog.NewLogLogger(log.Handler(), slog.LevelWarn),
	}

	go func() {
		log.Info("listening", "addr", cfg.Addr, "pg_max_conns", cfg.PGMaxConns,
			"gomaxprocs", runtime.GOMAXPROCS(0), "max_results", cfg.MaxResults,
			"cache", rc.Enabled(), "cache_ttl", cfg.CacheTTL)
		if err := server.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Error("server stopped", "err", err)
			os.Exit(1)
		}
	}()

	<-ctx.Done()
	log.Info("shutting down")
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer shutdownCancel()
	if err := server.Shutdown(shutdownCtx); err != nil {
		log.Error("shutdown", "err", err)
	}
}

func newLogger(level string) *slog.Logger {
	var lv slog.Level
	if err := lv.UnmarshalText([]byte(level)); err != nil {
		lv = slog.LevelInfo
	}
	return slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: lv}))
}
