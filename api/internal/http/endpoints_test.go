package httpapi_test

import (
	"encoding/json"
	"net/http"
	"testing"
	"time"

	"github.com/matthewlee/benchmark-platform/api/internal/model"
)

func fakeWithInventory() *fakeStore {
	f := newFake()
	f.machines = []model.MachineRow{{
		Machine: model.Machine{
			ID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", Hostname: "wsl-thinkpad",
			CPUModel: "Intel(R) Core(TM) Ultra 9 185H", PhysicalCores: 11, LogicalCPUs: 22,
		},
		FirstSeen: time.Now().Add(-48 * time.Hour), LastSeen: time.Now(), Runs: 3,
		Measurements: 1234,
	}}
	f.runs = []model.Run{
		{ID: "11111111-2222-4333-8444-555555555555", MachineID: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
		{ID: "11111111-2222-4333-8444-666666666666", MachineID: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
	}
	return f
}

func TestMachinesEndpoints(t *testing.T) {
	f := fakeWithInventory()
	h := newServer(t, f)

	rec := get(t, h, "/v1/machines")
	if rec.Code != http.StatusOK {
		t.Fatalf("status %d", rec.Code)
	}
	var list struct {
		Machines []model.MachineRow `json:"machines"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &list); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(list.Machines) != 1 || list.Machines[0].Measurements != 1234 {
		t.Fatalf("machines %+v", list.Machines)
	}

	rec = get(t, h, "/v1/machines/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
	if rec.Code != http.StatusOK {
		t.Fatalf("get machine: %d", rec.Code)
	}
	rec = get(t, h, "/v1/machines/does-not-exist")
	if rec.Code != http.StatusNotFound {
		t.Fatalf("unknown machine: %d, want 404", rec.Code)
	}
}

func TestRunsEndpoints(t *testing.T) {
	f := fakeWithInventory()
	h := newServer(t, f)

	rec := get(t, h, "/v1/runs")
	var list struct {
		Runs []model.Run `json:"runs"`
	}
	if err := json.Unmarshal(rec.Body.Bytes(), &list); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(list.Runs) != 2 {
		t.Fatalf("runs %+v", list.Runs)
	}

	rec = get(t, h, "/v1/runs?machine_id=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
	list.Runs = nil
	if err := json.Unmarshal(rec.Body.Bytes(), &list); err != nil {
		t.Fatalf("decode: %v", err)
	}
	if len(list.Runs) != 1 {
		t.Fatalf("filtered runs %+v", list.Runs)
	}

	if rec := get(t, h, "/v1/runs?limit=0"); rec.Code != http.StatusBadRequest {
		t.Fatalf("limit=0: %d, want 400", rec.Code)
	}
	if rec := get(t, h, "/v1/runs/11111111-2222-4333-8444-555555555555"); rec.Code != http.StatusOK {
		t.Fatalf("get run: %d", rec.Code)
	}
	if rec := get(t, h, "/v1/runs/nope"); rec.Code != http.StatusNotFound {
		t.Fatalf("unknown run: %d, want 404", rec.Code)
	}
}
