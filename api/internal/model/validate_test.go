package model_test

import (
	"encoding/json"
	"os"
	"strings"
	"testing"

	"github.com/santhosh-tekuri/jsonschema/v6"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

const examplePath = "../../../schema/examples/run.json"

func loadExample(t *testing.T) []byte {
	t.Helper()
	b, err := os.ReadFile(examplePath)
	if err != nil {
		t.Fatalf("read example: %v", err)
	}
	return b
}

func decode(t *testing.T, b []byte) model.RunEnvelope {
	t.Helper()
	var env model.RunEnvelope
	dec := json.NewDecoder(strings.NewReader(string(b)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&env); err != nil {
		t.Fatalf("decode: %v", err)
	}
	return env
}

func TestExampleRunValidates(t *testing.T) {
	env := decode(t, loadExample(t))
	if err := env.Validate(); err != nil {
		t.Fatalf("the repository's own example must validate: %v", err)
	}
	if len(env.Results) == 0 {
		t.Fatal("example has no results")
	}
}

// compiled returns the normative JSON Schema. The API does not use it at runtime (strict
// struct decoding plus these checks is both faster and gives better messages on a 50K
// envelope), so this test is the tie between the two: whatever the schema rejects, the Go
// validator must also reject.
func compiled(t *testing.T) *jsonschema.Schema {
	t.Helper()
	f, err := openSchema()
	if err != nil {
		t.Fatalf("open schema: %v", err)
	}
	defer f.Close()
	doc, err := jsonschema.UnmarshalJSON(f)
	if err != nil {
		t.Fatalf("unmarshal schema: %v", err)
	}
	c := jsonschema.NewCompiler()
	if err := c.AddResource("bench.json", doc); err != nil {
		t.Fatalf("add resource: %v", err)
	}
	sch, err := c.Compile("bench.json")
	if err != nil {
		t.Fatalf("compile schema: %v", err)
	}
	return sch
}

func schemaAccepts(t *testing.T, sch *jsonschema.Schema, raw []byte) bool {
	t.Helper()
	var v any
	if err := json.Unmarshal(raw, &v); err != nil {
		t.Fatalf("unmarshal instance: %v", err)
	}
	return sch.Validate(v) == nil
}

// mutations are one-field edits of the valid example. Each must be rejected by both the
// JSON Schema and the Go validator; "valid" is the control.
var mutations = []struct {
	name string
	edit func(map[string]any)
	// goRejects is false only where the Go validator legitimately cannot see the problem
	// (strict decoding catches it earlier, at decode time, not in Validate()).
	decodeFails bool
}{
	{name: "valid", edit: func(map[string]any) {}},
	{name: "schema_version 2", edit: func(d map[string]any) { d["schema_version"] = 2 }},
	{name: "run_id not a uuid", edit: func(d map[string]any) { d["run_id"] = "not-a-uuid" }},
	{name: "timestamp without Z", edit: func(d map[string]any) {
		d["results"].([]any)[0].(map[string]any)["timestamp"] = "2026-09-18T15:47:01.123456"
	}},
	{name: "unknown metric", edit: func(d map[string]any) {
		d["results"].([]any)[0].(map[string]any)["metric"] = "cpu_vibes"
	}},
	{name: "metric with the wrong unit", edit: func(d map[string]any) {
		d["results"].([]any)[0].(map[string]any)["unit"] = "GB/s"
	}},
	{name: "metric with the wrong workload", edit: func(d map[string]any) {
		d["results"].([]any)[0].(map[string]any)["workload"] = "mem_bw"
	}},
	{name: "thread_count 0", edit: func(d map[string]any) {
		d["results"].([]any)[0].(map[string]any)["thread_count"] = 0
	}},
	{name: "negative trial", edit: func(d map[string]any) {
		d["results"].([]any)[0].(map[string]any)["trial"] = -1
	}},
	{name: "duration_ns 0", edit: func(d map[string]any) {
		d["results"].([]any)[0].(map[string]any)["duration_ns"] = 0
	}},
	{name: "machine id too short", edit: func(d map[string]any) {
		d["machine"].(map[string]any)["id"] = "abc123"
	}},
	{name: "machine missing cpu_model", edit: func(d map[string]any) {
		delete(d["machine"].(map[string]any), "cpu_model")
	}},
	{name: "machine block inside a result", edit: func(d map[string]any) {
		d["results"].([]any)[0].(map[string]any)["machine"] = d["machine"]
	}, decodeFails: true},
	{name: "unknown top-level field", edit: func(d map[string]any) { d["oops"] = 1 }, decodeFails: true},
}

func TestGoValidatorAgreesWithJSONSchema(t *testing.T) {
	sch := compiled(t)
	base := loadExample(t)
	for _, m := range mutations {
		t.Run(m.name, func(t *testing.T) {
			var doc map[string]any
			if err := json.Unmarshal(base, &doc); err != nil {
				t.Fatalf("unmarshal base: %v", err)
			}
			m.edit(doc)
			raw, err := json.Marshal(doc)
			if err != nil {
				t.Fatalf("marshal mutation: %v", err)
			}
			wantValid := m.name == "valid"
			if got := schemaAccepts(t, sch, raw); got != wantValid {
				t.Fatalf("JSON Schema accepted=%v, want %v", got, wantValid)
			}

			var env model.RunEnvelope
			dec := json.NewDecoder(strings.NewReader(string(raw)))
			dec.DisallowUnknownFields()
			decErr := dec.Decode(&env)
			if m.decodeFails {
				if decErr == nil {
					t.Fatalf("strict decoding should have rejected %s", m.name)
				}
				return
			}
			if decErr != nil {
				t.Fatalf("decode: %v", decErr)
			}
			err = env.Validate()
			if wantValid && err != nil {
				t.Fatalf("Go validator rejected the valid document: %v", err)
			}
			if !wantValid && err == nil {
				t.Fatalf("Go validator accepted what the JSON Schema rejected")
			}
		})
	}
}

func TestValidateReportsEveryProblem(t *testing.T) {
	env := decode(t, loadExample(t))
	env.SchemaVersion = 7
	env.RunID = "nope"
	env.Results[0].Metric = "cpu_vibes"
	err := env.Validate()
	var ve *model.ValidationError
	if err == nil {
		t.Fatal("want a validation error")
	}
	if !asValidationError(err, &ve) {
		t.Fatalf("want *ValidationError, got %T", err)
	}
	if len(ve.Problems) < 3 {
		t.Fatalf("want at least 3 problems, got %v", ve.Problems)
	}
}

func asValidationError(err error, target **model.ValidationError) bool {
	ve, ok := err.(*model.ValidationError)
	if ok {
		*target = ve
	}
	return ok
}

func TestParseTimestamp(t *testing.T) {
	for _, tc := range []struct {
		in string
		ok bool
	}{
		{"2026-09-18T15:47:01.123456Z", true},
		{"2026-09-18T15:47:01Z", true},
		{"2026-09-18T15:47:01.123456789Z", true},
		{"2026-09-18T15:47:01.123456+02:00", false},
		{"2026-09-18 15:47:01Z", false},
		{"", false},
	} {
		_, err := model.ParseTimestamp(tc.in)
		if (err == nil) != tc.ok {
			t.Errorf("ParseTimestamp(%q): err=%v, want ok=%v", tc.in, err, tc.ok)
		}
	}
}

// A real engine envelope must survive strict decoding, not just the hand-written example.
// The file is committed engine output from the M1.3 verification run.
//
// This test exists because the first POST of a real run failed: the engine writes the
// M2.5 run-quality fields (late_trials, canary_attempts, clock_call_ns_*) into `summary`,
// the schema allows them, and the Go Summary struct did not have them, so
// DisallowUnknownFields rejected the document.
func TestRealEngineRunDecodesStrictly(t *testing.T) {
	const path = "../../../docs/results/m1.3/engine_run.json"
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	var env model.RunEnvelope
	dec := json.NewDecoder(strings.NewReader(string(b)))
	dec.DisallowUnknownFields()
	if err := dec.Decode(&env); err != nil {
		t.Fatalf("strict decode of real engine output: %v", err)
	}
	if err := env.Validate(); err != nil {
		t.Fatalf("real engine output failed validation: %v", err)
	}
	if len(env.Results) == 0 || len(env.Summary) == 0 {
		t.Fatalf("expected results and a summary, got %d/%d", len(env.Results), len(env.Summary))
	}
	if env.Summary[0].CanaryAttempts == 0 {
		t.Fatal("canary_attempts did not survive decoding")
	}
	if got := schemaAccepts(t, compiled(t), b); !got {
		t.Fatal("the normative JSON Schema rejects the engine's own output")
	}
}

// openSchema opens the normative schema file; shared with the benchmarks.
func openSchema() (*os.File, error) { return os.Open(schemaPath) }
