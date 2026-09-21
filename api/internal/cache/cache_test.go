package cache_test

import (
	"context"
	"io"
	"log/slog"
	"testing"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/cache"
)

func quiet() *slog.Logger { return slog.New(slog.NewTextHandler(io.Discard, nil)) }

// A cache pointed at a port nothing listens on is the "Redis is down" case: every call
// must fall through to the loader, and the breaker must stop the API paying a dial
// timeout per request forever.
func TestDownRedisFallsThroughAndTripsTheCircuit(t *testing.T) {
	c, err := cache.New(cache.Options{URL: "redis://127.0.0.1:1/0", TTL: time.Minute, Log: quiet()})
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()

	ctx := context.Background()
	calls := 0
	load := func() (any, error) {
		calls++
		return map[string]int{"n": calls}, nil
	}
	for i := 0; i < 10; i++ {
		body, hit, err := cache.Fetch(ctx, c, "k", []string{"m"}, load)
		if err != nil {
			t.Fatalf("request %d: %v", i, err)
		}
		if hit {
			t.Fatalf("request %d reported a cache hit from a dead Redis", i)
		}
		if len(body) == 0 {
			t.Fatalf("request %d returned no body", i)
		}
	}
	if calls != 10 {
		t.Fatalf("the loader ran %d times, want 10", calls)
	}
	st := c.Stats()
	if !st.CircuitOpen {
		t.Fatal("the circuit never opened; every request would keep paying a dial timeout")
	}
	if st.Errors == 0 {
		t.Fatal("errors were not counted")
	}
	// Once open, the cache must not be consulted at all, so the call is fast.
	start := time.Now()
	for i := 0; i < 20; i++ {
		if _, _, err := cache.Fetch(ctx, c, "k", []string{"m"}, load); err != nil {
			t.Fatal(err)
		}
	}
	if elapsed := time.Since(start); elapsed > 100*time.Millisecond {
		t.Fatalf("20 requests with the circuit open took %v; it is still dialling", elapsed)
	}
}

// A nil cache is the "no REDIS_URL" case and must behave the same way.
func TestNilCacheIsAlwaysAMiss(t *testing.T) {
	var c *cache.Cache
	if c.Enabled() {
		t.Fatal("a nil cache reports itself enabled")
	}
	body, hit, err := cache.Fetch(context.Background(), c, "k", nil, func() (any, error) {
		return map[string]string{"from": "postgres"}, nil
	})
	if err != nil || hit || len(body) == 0 {
		t.Fatalf("body=%s hit=%v err=%v", body, hit, err)
	}
	if n := c.InvalidateMachine(context.Background(), "m"); n != 0 {
		t.Fatalf("invalidate on a nil cache returned %d", n)
	}
	if err := c.Ping(context.Background()); err == nil {
		t.Fatal("a nil cache must not claim to be reachable")
	}
}

func TestNewRejectsABadURL(t *testing.T) {
	if _, err := cache.New(cache.Options{URL: "not-a-url"}); err == nil {
		t.Fatal("want an error for a malformed REDIS_URL")
	}
	c, err := cache.New(cache.Options{URL: ""})
	if err != nil || c != nil {
		t.Fatalf("an empty URL means no cache: %v %v", c, err)
	}
}
