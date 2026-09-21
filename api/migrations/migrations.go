// Package migrations applies the plain-SQL files next to this one.
//
// golang-migrate's CLI would do the same job; it is not installed on this machine and the
// rules it enforces (ordered, versioned, one transaction per file, recorded in
// schema_migrations) are 80 lines of Go. The files stay in golang-migrate's
// NNNN_name.up.sql / .down.sql convention so the CLI can take over unchanged.
package migrations

import (
	"context"
	"embed"
	"fmt"
	"io/fs"
	"sort"
	"strconv"
	"strings"

	"github.com/jackc/pgx/v5"
)

//go:embed *.sql
var files embed.FS

// Migration is one versioned pair of SQL files.
type Migration struct {
	Version int
	Name    string
	Up      string
	Down    string
}

// Load reads the embedded migrations, sorted by version.
func Load() ([]Migration, error) {
	entries, err := fs.ReadDir(files, ".")
	if err != nil {
		return nil, fmt.Errorf("read embedded migrations: %w", err)
	}
	byVersion := map[int]*Migration{}
	for _, e := range entries {
		name := e.Name()
		parts := strings.SplitN(name, "_", 2)
		if len(parts) != 2 {
			return nil, fmt.Errorf("migration %q: want NNNN_name.up.sql", name)
		}
		version, err := strconv.Atoi(parts[0])
		if err != nil {
			return nil, fmt.Errorf("migration %q: bad version: %w", name, err)
		}
		body, err := fs.ReadFile(files, name)
		if err != nil {
			return nil, err
		}
		m := byVersion[version]
		if m == nil {
			m = &Migration{Version: version}
			byVersion[version] = m
		}
		switch {
		case strings.HasSuffix(name, ".up.sql"):
			m.Name = strings.TrimSuffix(parts[1], ".up.sql")
			m.Up = string(body)
		case strings.HasSuffix(name, ".down.sql"):
			m.Down = string(body)
		default:
			return nil, fmt.Errorf("migration %q: neither .up.sql nor .down.sql", name)
		}
	}
	out := make([]Migration, 0, len(byVersion))
	for _, m := range byVersion {
		if m.Up == "" {
			return nil, fmt.Errorf("migration %d has no .up.sql", m.Version)
		}
		out = append(out, *m)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Version < out[j].Version })
	return out, nil
}

const createTable = `CREATE TABLE IF NOT EXISTS schema_migrations (
    version    integer PRIMARY KEY,
    name       text NOT NULL,
    applied_at timestamptz NOT NULL DEFAULT now()
)`

// Applied returns the versions already recorded in schema_migrations.
func Applied(ctx context.Context, conn *pgx.Conn) (map[int]bool, error) {
	if _, err := conn.Exec(ctx, createTable); err != nil {
		return nil, fmt.Errorf("create schema_migrations: %w", err)
	}
	rows, err := conn.Query(ctx, `SELECT version FROM schema_migrations`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[int]bool{}
	for rows.Next() {
		var v int
		if err := rows.Scan(&v); err != nil {
			return nil, err
		}
		out[v] = true
	}
	return out, rows.Err()
}

// Up applies every migration not yet recorded, each in its own transaction, and returns
// the versions it applied.
func Up(ctx context.Context, conn *pgx.Conn) ([]int, error) {
	ms, err := Load()
	if err != nil {
		return nil, err
	}
	applied, err := Applied(ctx, conn)
	if err != nil {
		return nil, err
	}
	var done []int
	for _, m := range ms {
		if applied[m.Version] {
			continue
		}
		if err := inTx(ctx, conn, func(tx pgx.Tx) error {
			if _, err := tx.Exec(ctx, m.Up); err != nil {
				return err
			}
			_, err := tx.Exec(ctx,
				`INSERT INTO schema_migrations (version, name) VALUES ($1, $2)`, m.Version, m.Name)
			return err
		}); err != nil {
			return done, fmt.Errorf("migration %04d_%s up: %w", m.Version, m.Name, err)
		}
		done = append(done, m.Version)
	}
	return done, nil
}

// Down rolls back the highest n applied migrations.
func Down(ctx context.Context, conn *pgx.Conn, n int) ([]int, error) {
	ms, err := Load()
	if err != nil {
		return nil, err
	}
	applied, err := Applied(ctx, conn)
	if err != nil {
		return nil, err
	}
	var done []int
	for i := len(ms) - 1; i >= 0 && len(done) < n; i-- {
		m := ms[i]
		if !applied[m.Version] {
			continue
		}
		if m.Down == "" {
			return done, fmt.Errorf("migration %04d_%s has no .down.sql", m.Version, m.Name)
		}
		if err := inTx(ctx, conn, func(tx pgx.Tx) error {
			if _, err := tx.Exec(ctx, m.Down); err != nil {
				return err
			}
			_, err := tx.Exec(ctx, `DELETE FROM schema_migrations WHERE version = $1`, m.Version)
			return err
		}); err != nil {
			return done, fmt.Errorf("migration %04d_%s down: %w", m.Version, m.Name, err)
		}
		done = append(done, m.Version)
	}
	return done, nil
}

func inTx(ctx context.Context, conn *pgx.Conn, fn func(pgx.Tx) error) error {
	tx, err := conn.Begin(ctx)
	if err != nil {
		return err
	}
	if err := fn(tx); err != nil {
		_ = tx.Rollback(ctx)
		return err
	}
	return tx.Commit(ctx)
}
