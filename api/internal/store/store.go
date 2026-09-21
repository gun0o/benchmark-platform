// Package store is the Postgres repository. One exported function per query, hand-written
// SQL, pgx/v5 with a pool.
package store

import (
	"context"
	"fmt"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

// Store owns the connection pool.
type Store struct {
	pool *pgxpool.Pool
}

// Config is what New needs; zero fields fall back to pgx's own defaults.
type Config struct {
	DSN      string
	MaxConns int32
	MinConns int32
}

// New opens the pool and verifies it can reach Postgres.
func New(ctx context.Context, cfg Config) (*Store, error) {
	pcfg, err := pgxpool.ParseConfig(cfg.DSN)
	if err != nil {
		return nil, fmt.Errorf("parse DATABASE_URL: %w", err)
	}
	if cfg.MaxConns > 0 {
		pcfg.MaxConns = cfg.MaxConns
	}
	if cfg.MinConns > 0 {
		pcfg.MinConns = cfg.MinConns
	}
	pool, err := pgxpool.NewWithConfig(ctx, pcfg)
	if err != nil {
		return nil, fmt.Errorf("open pool: %w", err)
	}
	if err := pool.Ping(ctx); err != nil {
		pool.Close()
		return nil, fmt.Errorf("ping postgres: %w", err)
	}
	return &Store{pool: pool}, nil
}

// Close releases the pool.
func (s *Store) Close() { s.pool.Close() }

// Pool exposes the pool for tests and for the pool-stats metrics.
func (s *Store) Pool() *pgxpool.Pool { return s.pool }

// Ping is what /readyz uses.
func (s *Store) Ping(ctx context.Context) error { return s.pool.Ping(ctx) }

// Stats reports pool counters (used by /metrics in M5.5 and by the load-test notes).
type Stats struct {
	AcquiredConns    int32         `json:"acquired_conns"`
	IdleConns        int32         `json:"idle_conns"`
	TotalConns       int32         `json:"total_conns"`
	MaxConns         int32         `json:"max_conns"`
	EmptyAcquires    int64         `json:"empty_acquire_count"`
	AcquireDuration  time.Duration `json:"acquire_duration"`
	CanceledAcquires int64         `json:"canceled_acquire_count"`
}

// Stats snapshots the pool's counters.
func (s *Store) Stats() Stats {
	st := s.pool.Stat()
	return Stats{
		AcquiredConns:    st.AcquiredConns(),
		IdleConns:        st.IdleConns(),
		TotalConns:       st.TotalConns(),
		MaxConns:         st.MaxConns(),
		EmptyAcquires:    st.EmptyAcquireCount(),
		AcquireDuration:  st.AcquireDuration(),
		CanceledAcquires: st.CanceledAcquireCount(),
	}
}

func (s *Store) inTx(ctx context.Context, fn func(pgx.Tx) error) error {
	tx, err := s.pool.Begin(ctx)
	if err != nil {
		return fmt.Errorf("begin: %w", err)
	}
	if err := fn(tx); err != nil {
		_ = tx.Rollback(ctx)
		return err
	}
	if err := tx.Commit(ctx); err != nil {
		return fmt.Errorf("commit: %w", err)
	}
	return nil
}
