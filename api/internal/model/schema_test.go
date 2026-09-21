package model_test

import (
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"testing"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

// schemaPath is the repository's normative schema, read at test time. Nothing in the Go
// code embeds it; these tests exist so the duplicated enum lists in model/enums.go cannot
// drift from it silently.
const schemaPath = "../../../schema/benchmark-result.schema.json"

func loadSchema(t *testing.T) map[string]any {
	t.Helper()
	b, err := os.ReadFile(filepath.Clean(schemaPath))
	if err != nil {
		t.Fatalf("read schema: %v", err)
	}
	var doc map[string]any
	if err := json.Unmarshal(b, &doc); err != nil {
		t.Fatalf("parse schema: %v", err)
	}
	return doc
}

func schemaEnum(t *testing.T, doc map[string]any, def string) []string {
	t.Helper()
	defs, ok := doc["$defs"].(map[string]any)
	if !ok {
		t.Fatalf("schema has no $defs")
	}
	node, ok := defs[def].(map[string]any)
	if !ok {
		t.Fatalf("schema $defs has no %q", def)
	}
	raw, ok := node["enum"].([]any)
	if !ok {
		t.Fatalf("$defs/%s has no enum", def)
	}
	out := make([]string, 0, len(raw))
	for _, v := range raw {
		out = append(out, v.(string))
	}
	return out
}

func TestEnumsMatchSchema(t *testing.T) {
	doc := loadSchema(t)
	for _, tc := range []struct {
		def  string
		have []string
	}{
		{"workload", model.Workloads},
		{"metric", model.Metrics},
		{"unit", model.Units},
	} {
		want := schemaEnum(t, doc, tc.def)
		if !reflect.DeepEqual(want, tc.have) {
			t.Errorf("%s enum drifted\n schema: %v\n     go: %v", tc.def, want, tc.have)
		}
	}
}

// The schema pins metric -> unit and metric -> workload in a list of if/then blocks.
// model.UnitOf and model.WorkloadOf restate them as maps; this reads the blocks back out
// and compares.
func TestMetricPairingsMatchSchema(t *testing.T) {
	doc := loadSchema(t)
	defs := doc["$defs"].(map[string]any)
	core := defs["result_core"].(map[string]any)
	blocks, ok := core["allOf"].([]any)
	if !ok {
		t.Fatalf("result_core has no allOf")
	}
	units := map[string]string{}
	workloads := map[string]string{}
	for _, b := range blocks {
		block := b.(map[string]any)
		ifProps := block["if"].(map[string]any)["properties"].(map[string]any)
		metric := ifProps["metric"].(map[string]any)["const"].(string)
		thenProps := block["then"].(map[string]any)["properties"].(map[string]any)
		units[metric] = thenProps["unit"].(map[string]any)["const"].(string)
		workloads[metric] = thenProps["workload"].(map[string]any)["const"].(string)
	}
	if !reflect.DeepEqual(units, model.UnitOf) {
		t.Errorf("metric -> unit drifted\n schema: %v\n     go: %v", units, model.UnitOf)
	}
	if !reflect.DeepEqual(workloads, model.WorkloadOf) {
		t.Errorf("metric -> workload drifted\n schema: %v\n     go: %v", workloads, model.WorkloadOf)
	}
	if len(units) != len(model.Metrics) {
		t.Errorf("schema pins %d metrics, the metric enum has %d", len(units), len(model.Metrics))
	}
}
