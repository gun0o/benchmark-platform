// api serves the benchmark REST API on API_ADDR (default :8080).
package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

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

	srv := httpapi.NewServer(st, httpapi.Options{Log: log, MaxResults: cfg.MaxResults})
	server := &http.Server{
		Addr:              cfg.Addr,
		Handler:           srv.Routes(),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       cfg.ReadTimeout,
		WriteTimeout:      cfg.WriteTimeout,
		ErrorLog:          slog.NewLogLogger(log.Handler(), slog.LevelWarn),
	}

	go func() {
		log.Info("listening", "addr", cfg.Addr, "pg_max_conns", cfg.PGMaxConns)
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
