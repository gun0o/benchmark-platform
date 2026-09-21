// Package config reads the process environment. Every knob has a default that works
// against the compose stack in deploy/docker-compose.yml.
package config

import (
	"os"
	"strconv"
	"time"
)

// Config is the API's runtime configuration.
type Config struct {
	Addr         string
	DatabaseURL  string
	RedisURL     string
	PGMaxConns   int32
	ReadTimeout  time.Duration
	WriteTimeout time.Duration
	HandlerLimit time.Duration
	MaxResults   int
	CacheTTL     time.Duration
	LogLevel     string
}

// Load reads the environment, applying defaults.
func Load() Config {
	return Config{
		Addr:        env("API_ADDR", ":8080"),
		DatabaseURL: env("DATABASE_URL", "postgres://bench:bench@localhost:5432/bench?sslmode=disable"),
		// An explicitly empty REDIS_URL means "no cache", which is different from the
		// variable being unset: the load test needs a way to turn the cache off without
		// stopping the Redis the rest of the stack shares.
		RedisURL:     envAllowEmpty("REDIS_URL", "redis://localhost:6379/0"),
		PGMaxConns:   int32(envInt("PG_MAX_CONNS", 20)),
		ReadTimeout:  envDuration("API_READ_TIMEOUT", 10*time.Second),
		WriteTimeout: envDuration("API_WRITE_TIMEOUT", 60*time.Second),
		HandlerLimit: envDuration("API_HANDLER_TIMEOUT", 5*time.Second),
		MaxResults:   envInt("API_MAX_RESULTS", 50_000),
		CacheTTL:     envDuration("CACHE_TTL", 60*time.Second),
		LogLevel:     env("LOG_LEVEL", "info"),
	}
}

func env(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

// envAllowEmpty returns the variable's value even when it is the empty string, and the
// default only when it is not set at all.
func envAllowEmpty(key, def string) string {
	if v, ok := os.LookupEnv(key); ok {
		return v
	}
	return def
}

func envInt(key string, def int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func envDuration(key string, def time.Duration) time.Duration {
	if v := os.Getenv(key); v != "" {
		if d, err := time.ParseDuration(v); err == nil {
			return d
		}
	}
	return def
}
