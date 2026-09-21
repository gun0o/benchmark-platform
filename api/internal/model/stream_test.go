package model_test

import (
	"bytes"
	"errors"
	"os"
	"strings"
	"testing"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

func collect(t *testing.T, doc string, maxResults int) (*model.RunEnvelope, []model.Result, error) {
	t.Helper()
	var got []model.Result
	env, err := model.StreamRunEnvelope(strings.NewReader(doc), maxResults,
		func(_ *model.RunEnvelope, _ int, r *model.Result) error {
			got = append(got, *r)
			return nil
		})
	return env, got, err
}

func TestStreamDecodesTheRealEngineRun(t *testing.T) {
	b, err := os.ReadFile("../../../docs/results/m1.3/engine_run.json")
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	env, got, err := collect(t, string(b), 50_000)
	if err != nil {
		t.Fatalf("stream: %v", err)
	}
	if len(got) != 10 {
		t.Fatalf("streamed %d results, want 10", len(got))
	}
	if env.RunID == "" || env.Machine.ID == "" || len(env.Summary) == 0 {
		t.Fatalf("header not filled in: %+v", env)
	}
	if env.Results != nil {
		t.Fatal("the streamed envelope must not also retain the results")
	}
}

// The callback must not see a result before the header that says where it belongs.
func TestStreamGivesTheHeaderBeforeAnyResult(t *testing.T) {
	b, err := os.ReadFile("../../../schema/examples/run.json")
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	seen := 0
	_, err = model.StreamRunEnvelope(bytes.NewReader(b), 100,
		func(env *model.RunEnvelope, i int, _ *model.Result) error {
			seen++
			if env.RunID == "" || env.Machine.ID == "" {
				return errors.New("callback ran before the header was complete")
			}
			if i != seen-1 {
				return errors.New("results arrived out of order")
			}
			return nil
		})
	if err != nil {
		t.Fatalf("stream: %v", err)
	}
	if seen != 3 {
		t.Fatalf("saw %d results, want 3", seen)
	}
}

// ...even when the document puts `results` first, which the schema permits: those results
// are buffered and replayed once the header is known.
func TestStreamHandlesResultsBeforeTheHeader(t *testing.T) {
	doc := `{"results":[
		{"workload":"cpu_int","thread_count":1,"working_set_bytes":0,"metric":"cpu_int_ops",
		 "value":1.0,"unit":"ops/s","trial":0,"timestamp":"2026-09-21T10:00:00.000000Z",
		 "duration_ns":1}],
		"schema_version":1,"run_id":"11111111-2222-4333-8444-555555555555",
		"started_at":"2026-09-21T10:00:00.000000Z","finished_at":"2026-09-21T10:00:01.000000Z",
		"machine":{"id":"ffffffffffffffffffffffffffffffff","hostname":"h","cpu_model":"c",
		 "physical_cores":1,"logical_cpus":1,"l1d_kb":0,"l2_kb":0,"l3_kb":0,"memory_bytes":0,
		 "os":"o","kernel":"k","compiler":"g","compiler_flags":"","engine_version":"1",
		 "engine_git_sha":"abc"},
		"argv":["bench"]}`
	env, got, err := collect(t, doc, 100)
	if err != nil {
		t.Fatalf("stream: %v", err)
	}
	if len(got) != 1 || env.RunID == "" {
		t.Fatalf("got %d results, env %+v", len(got), env)
	}
}

func TestStreamRejections(t *testing.T) {
	b, err := os.ReadFile("../../../schema/examples/run.json")
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	base := string(b)

	t.Run("too many results", func(t *testing.T) {
		_, _, err := collect(t, base, 2)
		var tooMany *model.TooManyResultsError
		if !errors.As(err, &tooMany) {
			t.Fatalf("err %v, want TooManyResultsError", err)
		}
	})

	t.Run("bad result stops the stream there", func(t *testing.T) {
		doc := strings.Replace(base, `"metric": "cpu_int_ops"`, `"metric": "cpu_vibes"`, 1)
		_, got, err := collect(t, doc, 100)
		var ve *model.ValidationError
		if !errors.As(err, &ve) {
			t.Fatalf("err %v, want ValidationError", err)
		}
		if len(got) != 0 {
			t.Fatalf("the callback saw %d results from a document that fails at result 0", len(got))
		}
	})

	t.Run("unknown top-level field", func(t *testing.T) {
		doc := strings.Replace(base, `"schema_version": 1,`, `"schema_version": 1, "oops": 1,`, 1)
		if _, _, err := collect(t, doc, 100); err == nil ||
			!strings.Contains(err.Error(), "unknown field") {
			t.Fatalf("err %v, want an unknown-field error", err)
		}
	})

	t.Run("unknown field inside a result", func(t *testing.T) {
		doc := strings.Replace(base, `"workload": "cpu_int",`, `"workload": "cpu_int", "oops": 1,`, 1)
		if _, _, err := collect(t, doc, 100); err == nil ||
			!strings.Contains(err.Error(), "unknown field") {
			t.Fatalf("err %v, want an unknown-field error", err)
		}
	})

	t.Run("truncated document", func(t *testing.T) {
		if _, _, err := collect(t, base[:len(base)/2], 100); err == nil {
			t.Fatal("a truncated envelope must not be accepted")
		}
	})

	t.Run("empty results", func(t *testing.T) {
		doc := `{"schema_version":1,"run_id":"11111111-2222-4333-8444-555555555555",
			"started_at":"2026-09-21T10:00:00.000000Z","finished_at":"2026-09-21T10:00:01.000000Z",
			"machine":{"id":"ffffffffffffffffffffffffffffffff","hostname":"h","cpu_model":"c",
			"physical_cores":1,"logical_cpus":1,"l1d_kb":0,"l2_kb":0,"l3_kb":0,"memory_bytes":0,
			"os":"o","kernel":"k","compiler":"g","compiler_flags":"","engine_version":"1",
			"engine_git_sha":"abc"},"argv":["bench"],"results":[]}`
		_, _, err := collect(t, doc, 100)
		var ve *model.ValidationError
		if !errors.As(err, &ve) {
			t.Fatalf("err %v, want a validation error for an empty run", err)
		}
	})
}

// The streaming path and the whole-document path must agree on what is valid.
func TestStreamAgreesWithValidate(t *testing.T) {
	sch := compiled(t)
	base := loadExample(t)
	for _, m := range mutations {
		t.Run(m.name, func(t *testing.T) {
			raw := mutate(t, base, m.edit)
			wantValid := m.name == "valid"
			if got := schemaAccepts(t, sch, raw); got != wantValid {
				t.Fatalf("JSON Schema accepted=%v, want %v", got, wantValid)
			}
			_, _, err := collect(t, string(raw), 1000)
			if wantValid && err != nil {
				t.Fatalf("streaming rejected the valid document: %v", err)
			}
			if !wantValid && err == nil {
				t.Fatal("streaming accepted what the JSON Schema rejected")
			}
		})
	}
}
