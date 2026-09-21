// M5.4 read-heavy profile: Target #4 is >= 1000 req/s at p95 <= 50 ms.
//
//   k6 run loadtest/read_heavy.js
//   API_BASE=http://api:8080 k6 run loadtest/read_heavy.js      (inside the compose network)
//   RATE=2000 k6 run loadtest/read_heavy.js                      (find the knee)
//
// Two scenarios. `warmup` runs first at a fifth of the rate so that the measured window
// does not start against a cold Redis and a cold Postgres buffer cache - the thresholds
// are scoped to `measured`, so the warmup's own latencies are reported but not judged.
import http from 'k6/http';
import { check } from 'k6';
import { Counter } from 'k6/metrics';
import { BASE, discover, pick } from './lib.js';

const RATE = Number(__ENV.RATE || 1000);
const DURATION = __ENV.DURATION || '60s';
const WARMUP = __ENV.WARMUP || '10s';

export const options = {
    discardResponseBodies: false,
    scenarios: {
        warmup: {
            executor: 'constant-arrival-rate',
            rate: Math.max(Math.round(RATE / 5), 1),
            timeUnit: '1s',
            duration: WARMUP,
            preAllocatedVUs: 50,
            maxVUs: 200,
            startTime: '0s',
            tags: { scenario: 'warmup' },
        },
        measured: {
            executor: 'constant-arrival-rate',
            rate: RATE,
            timeUnit: '1s',
            duration: DURATION,
            preAllocatedVUs: Number(__ENV.PRE_VUS || 100),
            maxVUs: Number(__ENV.MAX_VUS || 400),
            startTime: WARMUP,
            tags: { scenario: 'measured' },
        },
    },
    thresholds: {
        // Target #4, judged only over the measured window.
        'http_req_duration{scenario:measured}': ['p(95)<50'],
        'http_req_failed{scenario:measured}': ['rate<0.001'],
        // Proof that the arrival rate was actually achieved: a dropped iteration is k6
        // saying it could not start a request on time, which would make every latency
        // number above a measurement of a smaller load than the one claimed.
        dropped_iterations: ['count==0'],
        checks: ['rate>0.999'],
    },
};

const cacheHits = new Counter('cache_hits');
const cacheMisses = new Counter('cache_misses');

export function setup() {
    return discover();
}

export default function (data) {
    const c = pick(data.combos);
    const threads = pick(data.threads);
    const ws = pick(c.sets);
    const r = Math.random();

    let res;
    let name;
    if (r < 0.6) {
        name = 'aggregates';
        const groupBy = Math.random() < 0.5 && c.sets.length > 1 ? 'working_set_bytes' : 'thread_count';
        const pin = groupBy === 'thread_count' ? `working_set_bytes=${ws}` : `thread_count=${threads}`;
        res = http.get(
            `${BASE}/v1/aggregates?machine_id=${c.machine}&metric=${c.metric}&workload=${c.workload}&group_by=${groupBy}&${pin}`,
            { tags: { name } });
    } else if (r < 0.8) {
        name = 'compare';
        const others = data.machines.filter((m) => m !== c.machine);
        const list = [c.machine, pick(others)].join(',');
        res = http.get(`${BASE}/v1/compare?machines=${list}&metric=${c.metric}&workload=${c.workload}`,
            { tags: { name } });
    } else if (r < 0.95) {
        name = 'measurements';
        res = http.get(
            `${BASE}/v1/measurements?machine_id=${c.machine}&metric=${c.metric}&thread_count=${threads}&working_set_bytes=${ws}&limit=100`,
            { tags: { name } });
    } else {
        name = 'trials';
        res = http.get(
            `${BASE}/v1/trials?machine_id=${c.machine}&metric=${c.metric}&thread_count=${threads}&working_set_bytes=${ws}&limit=1000`,
            { tags: { name } });
    }

    check(res, { 'status is 200': (r2) => r2.status === 200 }, { name });
    const hdr = res.headers['X-Cache'];
    if (hdr === 'hit') cacheHits.add(1);
    else if (hdr === 'miss') cacheMisses.add(1);
}
