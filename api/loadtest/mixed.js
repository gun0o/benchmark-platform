// M5.4 mixed profile: the read-heavy load with ingest running underneath it.
//
//   k6 run loadtest/mixed.js
//
// This is the interesting one, because ingest invalidates the cache for the machine it
// writes to. The read profile alone measures a warm cache; this measures a cache that
// keeps being emptied while it is being read, which is what a live system does.
import { check } from 'k6';
import http from 'k6/http';
import { BASE, discover, pick } from './lib.js';
import readDefault from './read_heavy.js';
import ingestDefault from './ingest.js';

const RATE = Number(__ENV.RATE || 1000);
const INGEST_RATE = Number(__ENV.INGEST_RATE || 50);
const DURATION = __ENV.DURATION || '60s';
const WARMUP = __ENV.WARMUP || '10s';

export const options = {
    scenarios: {
        warmup: {
            executor: 'constant-arrival-rate',
            rate: Math.max(Math.round(RATE / 5), 1),
            timeUnit: '1s', duration: WARMUP,
            preAllocatedVUs: 50, maxVUs: 200, startTime: '0s',
            exec: 'read', tags: { scenario: 'warmup' },
        },
        measured: {
            executor: 'constant-arrival-rate',
            rate: RATE, timeUnit: '1s', duration: DURATION,
            preAllocatedVUs: 100, maxVUs: 400, startTime: WARMUP,
            exec: 'read', tags: { scenario: 'measured' },
        },
        ingest: {
            executor: 'constant-arrival-rate',
            rate: INGEST_RATE, timeUnit: '1s', duration: DURATION,
            preAllocatedVUs: 20, maxVUs: 100, startTime: WARMUP,
            exec: 'ingest', tags: { scenario: 'ingest' },
        },
    },
    thresholds: {
        'http_req_duration{scenario:measured}': ['p(95)<50'],
        'http_req_failed{scenario:measured}': ['rate<0.001'],
        dropped_iterations: ['count==0'],
    },
};

export function setup() {
    const data = discover();
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
    return { ...data, machine };
}

export function read(data) {
    readDefault(data);
}

export function ingest(data) {
    ingestDefault(data);
}

// Keep the linters quiet about the unused imports that document the mix.
export const _unused = { check, pick };
