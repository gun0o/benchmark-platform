// M5.4 ingest profile: 50 POSTs/s of 100-result envelopes, for the mixed run.
//
//   k6 run loadtest/ingest.js
//
// Each iteration invents a fresh run_id, so every POST is a real insert of 100 rows rather
// than a duplicate that the row-level conflict rule would skip. At 50/s for 60 s that is
// 300,000 rows, which is more than the read profile's dataset - the run is short and the
// table is expected to grow.
import http from 'k6/http';
import { check } from 'k6';
import { BASE, discover } from './lib.js';

const RATE = Number(__ENV.INGEST_RATE || 50);
const DURATION = __ENV.DURATION || '60s';

export const options = {
    scenarios: {
        ingest: {
            executor: 'constant-arrival-rate',
            rate: RATE,
            timeUnit: '1s',
            duration: DURATION,
            preAllocatedVUs: 20,
            maxVUs: 100,
            tags: { scenario: 'ingest' },
        },
    },
    thresholds: {
        'http_req_duration{scenario:ingest}': ['p(95)<500'],
        'http_req_failed{scenario:ingest}': ['rate<0.001'],
        dropped_iterations: ['count==0'],
    },
};

export function setup() {
    const data = discover();
    // The machine block has to come back exactly as the schema wants it, and the machines
    // endpoint does not carry engine_version (that belongs to a run), so it is filled in.
    const res = http.get(`${BASE}/v1/machines`);
    const m = res.json('machines')[0];
    const machine = {
        id: m.id, hostname: m.hostname, cpu_model: m.cpu_model,
        physical_cores: m.physical_cores, logical_cpus: m.logical_cpus,
        l1d_kb: m.l1d_kb, l2_kb: m.l2_kb, l3_kb: m.l3_kb, memory_bytes: m.memory_bytes,
        os: m.os, kernel: m.kernel, compiler: m.compiler, compiler_flags: m.compiler_flags,
        engine_version: 'loadtest', engine_git_sha: 'loadtest',
    };
    if (m.virtualized) machine.virtualized = m.virtualized;
    return { machine, combos: data.combos };
}

function hex(n) {
    let s = '';
    for (let i = 0; i < n; i++) s += '0123456789abcdef'[Math.floor(Math.random() * 16)];
    return s;
}

// uuid4 builds the v4-shaped id the schema's pattern requires.
function uuid4() {
    return `${hex(8)}-${hex(4)}-4${hex(3)}-${'89ab'[Math.floor(Math.random() * 4)]}${hex(3)}-${hex(12)}`;
}

export default function (data) {
    const now = new Date();
    const stamp = (d) => d.toISOString().replace('Z', '000Z');
    const results = [];
    for (let i = 0; i < 100; i++) {
        results.push({
            workload: 'cpu_int', thread_count: 1, working_set_bytes: 0,
            metric: 'cpu_int_ops', value: 4.6e9 + Math.random() * 1e8, unit: 'ops/s',
            trial: i, timestamp: stamp(new Date(now.getTime() + i)), duration_ns: 50000000,
            params: { cold: 'clflush', pinned: true, warmup_trials: 5, trial_ms: 50 },
        });
    }
    const body = JSON.stringify({
        schema_version: 1, run_id: uuid4(),
        started_at: stamp(now), finished_at: stamp(new Date(now.getTime() + 5000)),
        machine: data.machine, argv: ['k6', 'loadtest/ingest.js'], results,
    });
    const res = http.post(`${BASE}/v1/runs`, body, {
        headers: { 'Content-Type': 'application/json' },
        tags: { name: 'ingest' },
    });
    check(res, {
        'status is 200': (r) => r.status === 200,
        'inserted 100': (r) => r.status === 200 && r.json('inserted') === 100,
    });
}
