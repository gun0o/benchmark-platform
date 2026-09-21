Frequency used for ops/cycle: 4.83 GHz (measured, 1 thread).

### unpinned — best thread count per metric (Target #1)

| metric | best threads | aggregate ops/s | >= 1e7? | per-thread ops/s at 1 thread | ops/cycle at 1 thread |
|---|---:|---:|:--:|---:|---:|
| `cpu_int_ops` | 16 | 4.011e+10 | PASS | 4.647e+09 | 0.96 |
| `cpu_fp_ops` | 22 | 1.000e+11 | PASS | 9.070e+09 | 1.88 |
| `cpu_hash_ops` | 16 | 1.244e+09 | PASS | 1.536e+08 | 0.03 |

### unpinned

| metric | threads | aggregate ops/s | per-thread ops/s | scaling eff. | CoV | ops/cycle/thread |
|---|---:|---:|---:|---:|---:|---:|
| `cpu_int_ops` | 1 | 4.647e+09 | 4.647e+09 | 100% | 3.2% | 0.96 |
| `cpu_int_ops` | 2 | 8.795e+09 | 4.397e+09 | 95% | 8.6% | 0.91 |
| `cpu_int_ops` | 4 | 1.658e+10 | 4.146e+09 | 89% | 8.9% | 0.86 |
| `cpu_int_ops` | 8 | 2.866e+10 | 3.583e+09 | 77% | 6.6% | 0.74 |
| `cpu_int_ops` | 11 | 3.245e+10 | 2.950e+09 | 63% | 5.1% | 0.61 |
| `cpu_int_ops` | 16 | 4.011e+10 | 2.507e+09 | 54% | 3.4% | 0.52 |
| `cpu_int_ops` | 22 | 3.937e+10 | 1.790e+09 | 39% | 8.4% | 0.37 |
| `cpu_fp_ops` | 1 | 9.070e+09 | 9.070e+09 | 100% | 3.9% | 1.88 |
| `cpu_fp_ops` | 2 | 1.733e+10 | 8.665e+09 | 96% | 3.0% | 1.79 |
| `cpu_fp_ops` | 4 | 3.369e+10 | 8.423e+09 | 93% | 2.7% | 1.74 |
| `cpu_fp_ops` | 8 | 6.126e+10 | 7.658e+09 | 84% | 1.4% | 1.59 |
| `cpu_fp_ops` | 11 | 7.580e+10 | 6.891e+09 | 76% | 1.7% | 1.43 |
| `cpu_fp_ops` | 16 | 9.843e+10 | 6.152e+09 | 68% | 2.5% | 1.27 |
| `cpu_fp_ops` | 22 | 1.000e+11 | 4.548e+09 | 50% | 9.5% | 0.94 |
| `cpu_hash_ops` | 1 | 1.536e+08 | 1.536e+08 | 100% | 2.1% | 0.03 |
| `cpu_hash_ops` | 2 | 2.946e+08 | 1.473e+08 | 96% | 5.5% | 0.03 |
| `cpu_hash_ops` | 4 | 5.192e+08 | 1.298e+08 | 84% | 8.5% | 0.03 |
| `cpu_hash_ops` | 8 | 9.140e+08 | 1.143e+08 | 74% | 7.7% | 0.02 |
| `cpu_hash_ops` | 11 | 1.036e+09 | 9.419e+07 | 61% | 4.6% | 0.02 |
| `cpu_hash_ops` | 16 | 1.244e+09 | 7.776e+07 | 51% | 3.4% | 0.02 |
| `cpu_hash_ops` | 22 | 1.235e+09 | 5.612e+07 | 37% | 7.5% | 0.01 |

### pinned — best thread count per metric (Target #1)

| metric | best threads | aggregate ops/s | >= 1e7? | per-thread ops/s at 1 thread | ops/cycle at 1 thread |
|---|---:|---:|:--:|---:|---:|
| `cpu_int_ops` | 22 | 4.081e+10 | PASS | 4.593e+09 | 0.95 |
| `cpu_fp_ops` | 22 | 1.044e+11 | PASS | 9.461e+09 | 1.96 |
| `cpu_hash_ops` | 22 | 1.321e+09 | PASS | 1.463e+08 | 0.03 |

### pinned

| metric | threads | aggregate ops/s | per-thread ops/s | scaling eff. | CoV | ops/cycle/thread |
|---|---:|---:|---:|---:|---:|---:|
| `cpu_int_ops` | 1 | 4.593e+09 | 4.593e+09 | 100% | 4.7% | 0.95 |
| `cpu_int_ops` | 2 | 8.938e+09 | 4.469e+09 | 97% | 15.6% | 0.93 |
| `cpu_int_ops` | 4 | 1.672e+10 | 4.180e+09 | 91% | 8.0% | 0.87 |
| `cpu_int_ops` | 8 | 2.729e+10 | 3.411e+09 | 74% | 7.1% | 0.71 |
| `cpu_int_ops` | 11 | 3.210e+10 | 2.918e+09 | 64% | 5.5% | 0.60 |
| `cpu_int_ops` | 16 | 4.029e+10 | 2.518e+09 | 55% | 5.0% | 0.52 |
| `cpu_int_ops` | 22 | 4.081e+10 | 1.855e+09 | 40% | 9.0% | 0.38 |
| `cpu_fp_ops` | 1 | 9.461e+09 | 9.461e+09 | 100% | 3.0% | 1.96 |
| `cpu_fp_ops` | 2 | 1.808e+10 | 9.040e+09 | 96% | 3.4% | 1.87 |
| `cpu_fp_ops` | 4 | 3.389e+10 | 8.473e+09 | 90% | 3.0% | 1.75 |
| `cpu_fp_ops` | 8 | 6.082e+10 | 7.603e+09 | 80% | 2.8% | 1.57 |
| `cpu_fp_ops` | 11 | 7.547e+10 | 6.861e+09 | 73% | 1.4% | 1.42 |
| `cpu_fp_ops` | 16 | 9.733e+10 | 6.083e+09 | 64% | 1.2% | 1.26 |
| `cpu_fp_ops` | 22 | 1.044e+11 | 4.746e+09 | 50% | 7.0% | 0.98 |
| `cpu_hash_ops` | 1 | 1.463e+08 | 1.463e+08 | 100% | 2.9% | 0.03 |
| `cpu_hash_ops` | 2 | 2.895e+08 | 1.447e+08 | 99% | 3.7% | 0.03 |
| `cpu_hash_ops` | 4 | 5.061e+08 | 1.265e+08 | 87% | 9.0% | 0.03 |
| `cpu_hash_ops` | 8 | 9.438e+08 | 1.180e+08 | 81% | 4.5% | 0.02 |
| `cpu_hash_ops` | 11 | 1.072e+09 | 9.743e+07 | 67% | 5.6% | 0.02 |
| `cpu_hash_ops` | 16 | 1.250e+09 | 7.811e+07 | 53% | 3.7% | 0.02 |
| `cpu_hash_ops` | 22 | 1.321e+09 | 6.005e+07 | 41% | 5.8% | 0.01 |

