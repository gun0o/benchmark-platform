// Shared setup for the load-test profiles.
//
// The request mix is drawn from combinations that actually exist in the database, fetched
// once in setup(). A load test that asks for machine ids that are not there measures the
// speed of a 404, which is not the thing being claimed.
import http from 'k6/http';

export const BASE = __ENV.API_BASE || 'http://localhost:8080';

// The working sets the seeder sweeps, so a request names one that has rows behind it.
const BANDWIDTH_SETS = [32768, 262144, 2097152, 8388608, 67108864, 536870912];
const LATENCY_SETS = [4096, 32768, 131072, 524288, 2097152, 8388608, 33554432, 268435456, 1073741824];
const DISK_SETS = [1073741824, 2147483648, 4294967296];
const THREADS = [1, 2, 4, 8, 16];

const METRICS = [
    { metric: 'cpu_int_ops', workload: 'cpu_int', sets: [0] },
    { metric: 'cpu_fp_ops', workload: 'cpu_fp', sets: [0] },
    { metric: 'cpu_hash_ops', workload: 'cpu_hash', sets: [0] },
    { metric: 'mem_read_bw', workload: 'mem_bw', sets: BANDWIDTH_SETS },
    { metric: 'mem_write_bw', workload: 'mem_bw', sets: BANDWIDTH_SETS },
    { metric: 'mem_copy_bw', workload: 'mem_bw', sets: BANDWIDTH_SETS },
    { metric: 'mem_latency', workload: 'mem_latency', sets: LATENCY_SETS },
    { metric: 'disk_seq_read_bw', workload: 'disk_seq', sets: DISK_SETS },
    { metric: 'disk_seq_write_bw', workload: 'disk_seq', sets: DISK_SETS },
    { metric: 'disk_rand_read_iops', workload: 'disk_rand', sets: DISK_SETS },
    { metric: 'disk_rand_write_iops', workload: 'disk_rand', sets: DISK_SETS },
    { metric: 'disk_rand_read_p99_us', workload: 'disk_rand', sets: DISK_SETS },
];

// discover reads the machine inventory and builds the combinations the profiles draw from.
export function discover() {
    const res = http.get(`${BASE}/v1/machines`);
    if (res.status !== 200) {
        throw new Error(`GET /v1/machines: HTTP ${res.status}. Is the API up and the database seeded?`);
    }
    const machines = res.json('machines').map((m) => m.id);
    if (machines.length < 2) {
        throw new Error(`only ${machines.length} machines; run cmd/seed first`);
    }
    const combos = [];
    for (const id of machines) {
        for (const m of METRICS) {
            combos.push({ machine: id, metric: m.metric, workload: m.workload, sets: m.sets });
        }
    }
    return { machines, combos, threads: THREADS };
}

export function pick(arr) {
    return arr[Math.floor(Math.random() * arr.length)];
}
