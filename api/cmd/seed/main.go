// seed generates synthetic benchmark runs and either writes them or posts them.
//
//	go run ./cmd/seed --post http://localhost:8080
//	go run ./cmd/seed --machines 5 --trials 100 --seed 42 --out seed.json
//
// The output goes through the real ingest path, so the seeder also exercises it. Synthetic
// machines are labelled (hostname "synthetic-NN", engine_version "seed") so they can be
// filtered out of pages that show measured results.
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"time"

	"flag"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
	"github.com/matthewlee/benchmark-platform/api/internal/seed"
)

// minRows is the floor the milestone requires: the default invocation must produce at
// least this many measurement rows, and the command refuses to exit 0 below it.
const minRows = 100_000

func main() {
	def := seed.Defaults()
	var (
		machines   = flag.Int("machines", def.Machines, "synthetic machines to generate (1-5)")
		trials     = flag.Int("trials", def.Trials, "trials per configuration")
		seedVal    = flag.Uint64("seed", def.Seed, "RNG seed; the same seed gives the same document")
		runs       = flag.Int("runs-per-machine", def.RunsPerMachine, "split each machine's configurations across this many runs")
		out        = flag.String("out", "", "write the envelopes to this file (JSON array)")
		post       = flag.String("post", "", "POST each envelope to this API, e.g. http://localhost:8080")
		allowSmall = flag.Bool("allow-small", false, "do not fail when fewer than 100,000 rows are generated")
		chunk      = flag.Int("chunk", 50_000, "results per POST; larger runs are split, as the engine does")
		quiet      = flag.Bool("quiet", false, "only print the summary line")
	)
	flag.Parse()

	if *machines < 1 || *machines > len(seed.Profiles()) {
		fatal("--machines must be between 1 and %d", len(seed.Profiles()))
	}
	opts := seed.Options{
		Machines: *machines, Trials: *trials, Seed: *seedVal, RunsPerMachine: *runs,
	}
	rows := opts.Rows()
	fmt.Printf("seed: %d machines x %d configurations x %d trials = %d measurement rows\n",
		opts.Machines, seed.ConfigCount(), opts.Trials, rows)

	start := time.Now()
	envelopes := seed.Generate(opts)
	generated := 0
	for _, e := range envelopes {
		generated += len(e.Results)
	}
	genTime := time.Since(start)
	if generated != rows {
		fatal("internal error: generated %d rows, predicted %d", generated, rows)
	}
	fmt.Printf("seed: generated %d rows in %d runs in %v\n", generated, len(envelopes),
		genTime.Round(time.Millisecond))

	switch {
	case *out != "":
		if err := writeFile(*out, envelopes); err != nil {
			fatal("%v", err)
		}
		info, _ := os.Stat(*out)
		fmt.Printf("seed: wrote %s (%.1f MB)\n", *out, float64(info.Size())/1e6)
	case *post != "":
		inserted, err := postAll(*post, envelopes, *chunk, *quiet)
		if err != nil {
			fatal("%v", err)
		}
		fmt.Printf("seed: %d rows inserted, %d generated, total %v\n",
			inserted, generated, time.Since(start).Round(time.Millisecond))
	default:
		fmt.Println("seed: nothing to do; pass --out or --post")
	}

	if generated < minRows && !*allowSmall {
		fmt.Fprintf(os.Stderr,
			"seed: %d rows is below the %d-row floor; pass --allow-small if that is intended\n",
			generated, minRows)
		os.Exit(1)
	}
}

func writeFile(path string, envelopes []*model.RunEnvelope) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	enc := json.NewEncoder(f)
	// A JSON array of run envelopes: one document per `bench run`, which is what the API
	// ingests one at a time.
	if _, err := f.WriteString("[\n"); err != nil {
		return err
	}
	for i, e := range envelopes {
		if i > 0 {
			if _, err := f.WriteString(",\n"); err != nil {
				return err
			}
		}
		if err := enc.Encode(e); err != nil {
			return err
		}
	}
	_, err = f.WriteString("]\n")
	return err
}

func postAll(base string, envelopes []*model.RunEnvelope, chunk int, quiet bool) (int, error) {
	client := &http.Client{Timeout: 120 * time.Second}
	url := base
	if len(url) > 0 && url[len(url)-1] == '/' {
		url = url[:len(url)-1]
	}
	url += "/v1/runs"

	total := 0
	for i, env := range envelopes {
		for start := 0; start < len(env.Results); start += chunk {
			end := min(start+chunk, len(env.Results))
			part := *env
			part.Results = env.Results[start:end]
			body, err := json.Marshal(&part)
			if err != nil {
				return total, fmt.Errorf("marshal run %s: %w", env.RunID, err)
			}
			res, err := postOne(client, url, body)
			if err != nil {
				return total, err
			}
			total += res.Inserted
			if !quiet {
				fmt.Printf("seed: run %d/%d %s -> %d inserted, %d skipped\n",
					i+1, len(envelopes), env.RunID, res.Inserted, res.Skipped)
			}
		}
	}
	return total, nil
}

type ingestResponse struct {
	Inserted int `json:"inserted"`
	Skipped  int `json:"skipped"`
}

func postOne(client *http.Client, url string, body []byte) (ingestResponse, error) {
	var out ingestResponse
	resp, err := client.Post(url, "application/json", bytes.NewReader(body))
	if err != nil {
		return out, fmt.Errorf("post: %w", err)
	}
	defer resp.Body.Close()
	dec := json.NewDecoder(resp.Body)
	if resp.StatusCode != http.StatusOK {
		var errBody map[string]any
		_ = dec.Decode(&errBody)
		return out, fmt.Errorf("post: HTTP %d: %v", resp.StatusCode, errBody)
	}
	if err := dec.Decode(&out); err != nil {
		return out, fmt.Errorf("decode response: %w", err)
	}
	return out, nil
}

func fatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "seed: "+format+"\n", args...)
	os.Exit(1)
}
