package model_test

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strings"
	"testing"

	"github.com/santhosh-tekuri/jsonschema/v6"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

// bigEnvelope builds a valid envelope with n results, the shape a 1000-trial sweep POSTs.
func bigEnvelope(n int) []byte {
	var b bytes.Buffer
	b.WriteString(`{"schema_version":1,"run_id":"11111111-2222-4333-8444-555555555555",` +
		`"started_at":"2026-09-21T10:00:00.000000Z","finished_at":"2026-09-21T10:30:00.000000Z",` +
		`"machine":{"id":"ffffffffffffffffffffffffffffffff","hostname":"bench","cpu_model":"cpu",` +
		`"physical_cores":11,"logical_cpus":22,"l1d_kb":48,"l2_kb":2048,"l3_kb":24576,` +
		`"memory_bytes":16659374080,"os":"linux","kernel":"6.18","compiler":"g++ 13.4.0",` +
		`"compiler_flags":"-O3 -march=native","engine_version":"0.1.0","engine_git_sha":"abc1234"},` +
		`"argv":["bench","run"],"results":[`)
	for i := 0; i < n; i++ {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, `{"workload":"cpu_int","thread_count":8,"working_set_bytes":0,`+
			`"metric":"cpu_int_ops","value":%d.5,"unit":"ops/s","trial":%d,`+
			`"timestamp":"2026-09-21T10:%02d:%02d.123456Z","duration_ns":50123456,`+
			`"params":{"cold":"clflush","pinned":true,"warmup_trials":5,"trial_ms":50}}`,
			12345678+i, i, (i/60)%60, i%60)
	}
	b.WriteString(`]}`)
	return b.Bytes()
}

const benchResults = 50_000

// BenchmarkStrictDecodeAndValidate is the path POST /v1/runs actually takes.
func BenchmarkStrictDecodeAndValidate(b *testing.B) {
	raw := bigEnvelope(benchResults)
	b.SetBytes(int64(len(raw)))
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		var env model.RunEnvelope
		dec := json.NewDecoder(strings.NewReader(string(raw)))
		dec.DisallowUnknownFields()
		if err := dec.Decode(&env); err != nil {
			b.Fatal(err)
		}
		if err := env.Validate(); err != nil {
			b.Fatal(err)
		}
	}
}

// BenchmarkJSONSchemaValidate is the alternative M1.3 did not take: compile the normative
// schema and validate the decoded document against it at request time.
func BenchmarkJSONSchemaValidate(b *testing.B) {
	raw := bigEnvelope(benchResults)
	f, err := openSchema()
	if err != nil {
		b.Fatal(err)
	}
	defer f.Close()
	doc, err := jsonschema.UnmarshalJSON(f)
	if err != nil {
		b.Fatal(err)
	}
	c := jsonschema.NewCompiler()
	if err := c.AddResource("bench.json", doc); err != nil {
		b.Fatal(err)
	}
	sch, err := c.Compile("bench.json")
	if err != nil {
		b.Fatal(err)
	}
	b.SetBytes(int64(len(raw)))
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		var v any
		if err := json.Unmarshal(raw, &v); err != nil {
			b.Fatal(err)
		}
		if err := sch.Validate(v); err != nil {
			b.Fatal(err)
		}
	}
}
