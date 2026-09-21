Frequency used for ops/cycle: 4.28 GHz (measured, 1 thread).

### unpinned — best thread count per metric (Target #1)

| metric | best threads | aggregate ops/s | >= 1e7? | per-thread ops/s at 1 thread | ops/cycle at 1 thread |
|---|---:|---:|:--:|---:|---:|
| `cpu_int_ops` | 16 | 3.711e+10 | PASS | 3.862e+09 | 0.90 |
| `cpu_fp_ops` | 22 | 8.587e+10 | PASS | 7.908e+09 | 1.85 |
| `cpu_hash_ops` | 16 | 1.083e+09 | PASS | 1.280e+08 | 0.03 |

### unpinned

| metric | threads | aggregate ops/s | per-thread ops/s | scaling eff. | CoV | ops/cycle/thread |
|---|---:|---:|---:|---:|---:|---:|
| `cpu_int_ops` | 1 | 3.862e+09 | 3.862e+09 | 100% | 15.0% | 0.90 |
| `cpu_int_ops` | 2 | 8.072e+09 | 4.036e+09 | 104% | 8.7% | 0.94 |
| `cpu_int_ops` | 4 | 1.660e+10 | 4.151e+09 | 107% | 5.4% | 0.97 |
| `cpu_int_ops` | 8 | 2.821e+10 | 3.526e+09 | 91% | 5.7% | 0.82 |
| `cpu_int_ops` | 11 | 3.413e+10 | 3.103e+09 | 80% | 4.8% | 0.72 |
| `cpu_int_ops` | 16 | 3.711e+10 | 2.320e+09 | 60% | 5.1% | 0.54 |
| `cpu_int_ops` | 22 | 3.299e+10 | 1.499e+09 | 39% | 10.9% | 0.35 |
| `cpu_fp_ops` | 1 | 7.908e+09 | 7.908e+09 | 100% | 8.6% | 1.85 |
| `cpu_fp_ops` | 2 | 1.574e+10 | 7.869e+09 | 100% | 11.2% | 1.84 |
| `cpu_fp_ops` | 4 | 3.276e+10 | 8.190e+09 | 104% | 5.8% | 1.91 |
| `cpu_fp_ops` | 8 | 5.625e+10 | 7.032e+09 | 89% | 4.0% | 1.64 |
| `cpu_fp_ops` | 11 | 6.744e+10 | 6.131e+09 | 78% | 3.9% | 1.43 |
| `cpu_fp_ops` | 16 | 8.501e+10 | 5.313e+09 | 67% | 3.2% | 1.24 |
| `cpu_fp_ops` | 22 | 8.587e+10 | 3.903e+09 | 49% | 7.1% | 0.91 |
| `cpu_hash_ops` | 1 | 1.280e+08 | 1.280e+08 | 100% | 12.3% | 0.03 |
| `cpu_hash_ops` | 2 | 2.634e+08 | 1.317e+08 | 103% | 8.5% | 0.03 |
| `cpu_hash_ops` | 4 | 5.457e+08 | 1.364e+08 | 107% | 5.7% | 0.03 |
| `cpu_hash_ops` | 8 | 8.629e+08 | 1.079e+08 | 84% | 4.7% | 0.03 |
| `cpu_hash_ops` | 11 | 9.960e+08 | 9.055e+07 | 71% | 3.9% | 0.02 |
| `cpu_hash_ops` | 16 | 1.083e+09 | 6.766e+07 | 53% | 6.9% | 0.02 |
| `cpu_hash_ops` | 22 | 1.064e+09 | 4.836e+07 | 38% | 7.1% | 0.01 |

### pinned — best thread count per metric (Target #1)

| metric | best threads | aggregate ops/s | >= 1e7? | per-thread ops/s at 1 thread | ops/cycle at 1 thread |
|---|---:|---:|:--:|---:|---:|
| `cpu_int_ops` | 22 | 3.398e+10 | PASS | 3.970e+09 | 0.93 |
| `cpu_fp_ops` | 22 | 8.099e+10 | PASS | 7.524e+09 | 1.76 |
| `cpu_hash_ops` | 16 | 1.101e+09 | PASS | 1.150e+08 | 0.03 |

### pinned

| metric | threads | aggregate ops/s | per-thread ops/s | scaling eff. | CoV | ops/cycle/thread |
|---|---:|---:|---:|---:|---:|---:|
| `cpu_int_ops` | 1 | 3.970e+09 | 3.970e+09 | 100% | 12.3% | 0.93 |
| `cpu_int_ops` | 2 | 8.318e+09 | 4.159e+09 | 105% | 15.1% | 0.97 |
| `cpu_int_ops` | 4 | 1.687e+10 | 4.218e+09 | 106% | 5.6% | 0.99 |
| `cpu_int_ops` | 8 | 2.933e+10 | 3.666e+09 | 92% | 5.4% | 0.86 |
| `cpu_int_ops` | 11 | 3.345e+10 | 3.041e+09 | 77% | 3.6% | 0.71 |
| `cpu_int_ops` | 16 | 3.365e+10 | 2.103e+09 | 53% | 6.2% | 0.49 |
| `cpu_int_ops` | 22 | 3.398e+10 | 1.545e+09 | 39% | 7.2% | 0.36 |
| `cpu_fp_ops` | 1 | 7.524e+09 | 7.524e+09 | 100% | 10.2% | 1.76 |
| `cpu_fp_ops` | 2 | 1.569e+10 | 7.846e+09 | 104% | 10.2% | 1.83 |
| `cpu_fp_ops` | 4 | 3.244e+10 | 8.109e+09 | 108% | 10.5% | 1.89 |
| `cpu_fp_ops` | 8 | 5.294e+10 | 6.618e+09 | 88% | 6.9% | 1.55 |
| `cpu_fp_ops` | 11 | 6.229e+10 | 5.663e+09 | 75% | 4.4% | 1.32 |
| `cpu_fp_ops` | 16 | 7.669e+10 | 4.793e+09 | 64% | 4.4% | 1.12 |
| `cpu_fp_ops` | 22 | 8.099e+10 | 3.681e+09 | 49% | 6.7% | 0.86 |
| `cpu_hash_ops` | 1 | 1.150e+08 | 1.150e+08 | 100% | 11.0% | 0.03 |
| `cpu_hash_ops` | 2 | 2.455e+08 | 1.227e+08 | 107% | 7.7% | 0.03 |
| `cpu_hash_ops` | 4 | 4.994e+08 | 1.249e+08 | 109% | 5.3% | 0.03 |
| `cpu_hash_ops` | 8 | 8.260e+08 | 1.032e+08 | 90% | 4.4% | 0.02 |
| `cpu_hash_ops` | 11 | 1.041e+09 | 9.461e+07 | 82% | 3.7% | 0.02 |
| `cpu_hash_ops` | 16 | 1.101e+09 | 6.879e+07 | 60% | 4.0% | 0.02 |
| `cpu_hash_ops` | 22 | 1.078e+09 | 4.901e+07 | 43% | 9.4% | 0.01 |

