# Root shortcuts. Sub-project details live in engine/, api/, dashboard/.
# The toolchain is user-local on this machine (see PLAN.md M0.1).
export PATH := $(HOME)/.local/opt/go/bin:$(HOME)/go/bin:$(HOME)/.local/bin:$(PATH)

COMPOSE := docker compose -f deploy/docker-compose.yml
SCHEMA  := schema/benchmark-result.schema.json

.PHONY: help up down logs ps test test-engine test-api test-dashboard bench seed load validate-schema

help:
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-16s %s\n", $$1, $$2}'

up: ## Start postgres + redis (+ api + dashboard once they exist)
	$(COMPOSE) up -d --wait

down: ## Stop and remove containers and volumes
	$(COMPOSE) down -v

logs: ## Tail compose logs
	$(COMPOSE) logs -f

ps: ## Show compose service status
	$(COMPOSE) ps

test: test-engine test-api test-dashboard ## Run every component's tests

test-engine: ## Build + ctest the engine (release preset)
	@if [ -f engine/CMakeLists.txt ]; then \
		cd engine && cmake --preset release >/dev/null && cmake --build --preset release -j >/dev/null && ctest --preset release; \
	else echo "engine: not yet"; fi

test-api: ## go test the API
	@if [ -f api/go.mod ]; then cd api && go test ./...; else echo "api: not yet"; fi

test-dashboard: ## vitest + typecheck the dashboard
	@if [ -f dashboard/package.json ]; then cd dashboard && npm run typecheck && npm run test; else echo "dashboard: not yet"; fi

bench: ## Run the engine natively and post to the local API
	@if [ -x engine/build/release/bench ]; then engine/build/release/bench run --all --post http://localhost:8080; else echo "bench: engine not built yet (make test-engine)"; fi

seed: ## Generate and ingest 100K+ synthetic measurements
	@if [ -f api/go.mod ]; then cd api && go run ./cmd/seed --post http://localhost:8080; else echo "seed: not yet"; fi

load: ## k6 read-heavy load test against the local API
	@if [ -f api/loadtest/read_heavy.js ]; then k6 run api/loadtest/read_heavy.js; else echo "load: not yet"; fi

validate-schema: ## Validate schema/examples/*.json against the result schema
	@for f in schema/examples/*.json; do \
		check-jsonschema --schemafile $(SCHEMA) $$f && echo "valid: $$f"; \
	done
