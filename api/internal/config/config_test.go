package config_test

import (
	"testing"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/config"
)

func TestDefaults(t *testing.T) {
	for _, key := range []string{"API_ADDR", "DATABASE_URL", "REDIS_URL", "PG_MAX_CONNS",
		"API_HANDLER_TIMEOUT", "CACHE_TTL"} {
		t.Setenv(key, "")
	}
	// t.Setenv sets them to empty, which is "set" - unset them properly by using a clean
	// lookup on the two that distinguish the cases.
	cfg := config.Load()
	if cfg.Addr != ":8080" {
		t.Fatalf("API_ADDR default %q", cfg.Addr)
	}
	if cfg.PGMaxConns != 20 {
		t.Fatalf("PG_MAX_CONNS default %d", cfg.PGMaxConns)
	}
	if cfg.CacheTTL != 60*time.Second {
		t.Fatalf("CACHE_TTL default %v", cfg.CacheTTL)
	}
}

// An explicitly empty REDIS_URL means "run without a cache", which is different from the
// variable not being set at all. The load test needs that distinction to measure the
// cache's contribution without stopping the Redis the rest of the stack shares.
func TestEmptyRedisURLDisablesTheCache(t *testing.T) {
	t.Setenv("REDIS_URL", "")
	if got := config.Load().RedisURL; got != "" {
		t.Fatalf("REDIS_URL= gave %q, want the empty string", got)
	}
	t.Setenv("REDIS_URL", "redis://elsewhere:6379/1")
	if got := config.Load().RedisURL; got != "redis://elsewhere:6379/1" {
		t.Fatalf("REDIS_URL was ignored: %q", got)
	}
}

func TestOverridesAreRead(t *testing.T) {
	t.Setenv("API_ADDR", ":9999")
	t.Setenv("PG_MAX_CONNS", "40")
	t.Setenv("CACHE_TTL", "5s")
	t.Setenv("API_MAX_RESULTS", "1234")
	cfg := config.Load()
	if cfg.Addr != ":9999" || cfg.PGMaxConns != 40 || cfg.CacheTTL != 5*time.Second ||
		cfg.MaxResults != 1234 {
		t.Fatalf("overrides not applied: %+v", cfg)
	}
	t.Setenv("PG_MAX_CONNS", "not-a-number")
	if config.Load().PGMaxConns != 20 {
		t.Fatal("an unparseable PG_MAX_CONNS must fall back to the default")
	}
}
