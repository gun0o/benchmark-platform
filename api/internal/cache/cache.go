// Package cache is the Redis cache-aside layer for the aggregate, compare and trials
// endpoints.
//
// Two rules shape it:
//
//   - The API must work with Redis down. Every miss, error and timeout falls through to
//     Postgres; a cache is an optimization, and an optimization that can take the service
//     down is a liability. Errors are logged once a minute rather than once a request, so
//     a dead Redis cannot also fill the disk with logs.
//   - A cached answer must be invalidated by the thing that makes it wrong, which is an
//     ingest for the machine it describes. Every key is registered in a per-machine set,
//     and ingest deletes that machine's set. Machine A's ingest never evicts machine B.
package cache

import (
	"context"
	"encoding/json"
	"errors"
	"log/slog"
	"sync"
	"time"

	"github.com/redis/go-redis/v9"
)

// Cache wraps a Redis client. A nil *Cache is valid and behaves as "always a miss", which
// is what the API uses when REDIS_URL is empty.
type Cache struct {
	rdb *redis.Client
	ttl time.Duration
	log *slog.Logger

	mu           sync.Mutex
	lastLogged   time.Time
	suppressed   int
	hits, misses int64
	errs         int64
	// Circuit breaker. After failures consecutive errors the cache is skipped entirely
	// until openUntil, because talking to a Redis that is not there costs a dial timeout
	// per operation and makes every request slower than having no cache at all.
	consecutive int
	openUntil   time.Time
}

// circuit constants: three strikes, then thirty seconds of not asking.
const (
	circuitThreshold = 3
	circuitCooldown  = 30 * time.Second
)

// Options configure New.
type Options struct {
	URL string
	TTL time.Duration
	Log *slog.Logger
}

// New parses the URL and returns a Cache. It does not connect; the first operation does,
// and a failure there is a miss, not an error the caller has to handle.
func New(opts Options) (*Cache, error) {
	if opts.URL == "" {
		return nil, nil
	}
	ropt, err := redis.ParseURL(opts.URL)
	if err != nil {
		return nil, err
	}
	// Short timeouts and no retries: the cache exists to make requests faster, so waiting
	// on it longer than the query it replaces defeats the purpose, and retrying a Redis
	// that is down multiplies the wait by the retry count.
	ropt.DialTimeout = 200 * time.Millisecond
	ropt.ReadTimeout = 200 * time.Millisecond
	ropt.WriteTimeout = 200 * time.Millisecond
	ropt.MaxRetries = -1 // go-redis: -1 disables retries, 0 means "use the default of 3"
	ttl := opts.TTL
	if ttl <= 0 {
		ttl = 60 * time.Second
	}
	log := opts.Log
	if log == nil {
		log = slog.Default()
	}
	return &Cache{rdb: redis.NewClient(ropt), ttl: ttl, log: log}, nil
}

// Close releases the client.
func (c *Cache) Close() error {
	if c == nil || c.rdb == nil {
		return nil
	}
	return c.rdb.Close()
}

// Ping reports whether Redis is reachable; /readyz uses it to say "degraded".
func (c *Cache) Ping(ctx context.Context) error {
	if c == nil || c.rdb == nil {
		return errors.New("cache disabled")
	}
	return c.rdb.Ping(ctx).Err()
}

// Enabled reports whether a cache is configured at all.
func (c *Cache) Enabled() bool { return c != nil && c.rdb != nil }

// Stats is a snapshot of the counters, for /metrics and the tests.
type Stats struct {
	Hits        int64 `json:"hits"`
	Misses      int64 `json:"misses"`
	Errors      int64 `json:"errors"`
	CircuitOpen bool  `json:"circuit_open"`
}

// Stats returns the counters.
func (c *Cache) Stats() Stats {
	if c == nil {
		return Stats{}
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	return Stats{Hits: c.hits, Misses: c.misses, Errors: c.errs,
		CircuitOpen: time.Now().Before(c.openUntil)}
}

// up reports whether Redis should be consulted at all: it is enabled, and the circuit is
// not open.
func (c *Cache) up() bool {
	if !c.Enabled() {
		return false
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	return time.Now().After(c.openUntil)
}

// ok records a successful operation, which closes the circuit.
func (c *Cache) ok() {
	c.mu.Lock()
	c.consecutive = 0
	c.mu.Unlock()
}

// note records a Redis failure, trips the circuit after enough of them in a row, and logs
// at most one line per minute.
func (c *Cache) note(op string, err error) {
	c.mu.Lock()
	c.errs++
	c.consecutive++
	tripped := false
	if c.consecutive >= circuitThreshold {
		c.openUntil = time.Now().Add(circuitCooldown)
		c.consecutive = 0
		tripped = true
	}
	now := time.Now()
	if now.Sub(c.lastLogged) < time.Minute {
		c.suppressed++
		c.mu.Unlock()
		return
	}
	suppressed := c.suppressed
	c.suppressed = 0
	c.lastLogged = now
	c.mu.Unlock()
	c.log.Warn("redis unavailable, serving from postgres",
		"op", op, "err", err, "suppressed_since_last", suppressed,
		"circuit_open_for", circuitCooldown.String(), "tripped", tripped)
}

func (c *Cache) count(hit bool) {
	c.mu.Lock()
	if hit {
		c.hits++
	} else {
		c.misses++
	}
	c.mu.Unlock()
}

// machineKeysKey is the set of cache keys that describe one machine's data.
func machineKeysKey(machineID string) string { return "keys:machine:" + machineID }

// Fetch is the cache-aside path: return the cached JSON for key, or call load, store the
// result under key, register it against every machine it depends on, and return it.
//
// The value is cached as the JSON the handler will write, so a hit costs one Redis round
// trip and no re-encoding.
func Fetch(ctx context.Context, c *Cache, key string, machines []string, load func() (any, error)) ([]byte, bool, error) {
	if !c.up() {
		v, err := load()
		if err != nil {
			return nil, false, err
		}
		b, err := json.Marshal(v)
		return b, false, err
	}

	if b, err := c.rdb.Get(ctx, key).Bytes(); err == nil {
		c.ok()
		c.count(true)
		return b, true, nil
	} else if errors.Is(err, redis.Nil) {
		c.ok() // a miss is a healthy Redis saying "not here"
	} else {
		c.note("get", err)
	}
	c.count(false)

	v, err := load()
	if err != nil {
		return nil, false, err
	}
	b, err := json.Marshal(v)
	if err != nil {
		return nil, false, err
	}

	pipe := c.rdb.TxPipeline()
	pipe.Set(ctx, key, b, c.ttl)
	for _, m := range machines {
		if m == "" {
			continue
		}
		pipe.SAdd(ctx, machineKeysKey(m), key)
		// The set must not outlive the keys it tracks, or it grows forever.
		pipe.Expire(ctx, machineKeysKey(m), c.ttl*10)
	}
	if _, err := pipe.Exec(ctx); err != nil {
		c.note("set", err) // the answer is still correct; it just was not cached
	} else {
		c.ok()
	}
	return b, false, nil
}

// InvalidateMachine deletes every cached answer that depends on one machine, and nothing
// else. Called after an ingest.
func (c *Cache) InvalidateMachine(ctx context.Context, machineID string) int {
	if !c.up() || machineID == "" {
		return 0
	}
	setKey := machineKeysKey(machineID)
	keys, err := c.rdb.SMembers(ctx, setKey).Result()
	if err != nil {
		c.note("smembers", err)
		return 0
	}
	if len(keys) == 0 {
		return 0
	}
	pipe := c.rdb.TxPipeline()
	pipe.Del(ctx, keys...)
	pipe.Del(ctx, setKey)
	if _, err := pipe.Exec(ctx); err != nil {
		c.note("del", err)
		return 0
	}
	c.ok()
	return len(keys)
}
