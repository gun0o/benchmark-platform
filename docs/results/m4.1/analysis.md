## Verify 2a: sequential 1 MiB O_DIRECT reads

| threads | bench MB/s | fio MB/s | difference |
|---:|---:|---:|---:|
| 1 | 2690 | 2688 | +0.1 % |
| 4 | 6405 | 5449 | +17.5 % |

## Verify 2b: random 4 KiB O_DIRECT reads, queue depth = threads

| threads (QD) | bench IOPS | fio IOPS | diff | bench p99 us | fio p99 us | diff |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 9131 | 8120 | +12.5 % | 274 | 285 | -3.7 % |
| 4 | 32808 | 29219 | +12.3 % | 234 | 276 | -15.4 % |
| 16 | 89966 | 83696 | +7.5 % | 300 | 305 | -1.7 % |

## Write metrics (no fio comparison: PLAN.md asks for reads)

| metric | threads | median | unit | CoV % |
|---|---:|---:|---|---:|
| `disk_seq_write_bw` | 1 | 1487 | MB/s | 1.9 |
| `disk_seq_write_bw` | 4 | 1951 | MB/s | 1.5 |
| `disk_rand_write_iops` | 1 | 512 | IOPS | 8.1 |
| `disk_rand_write_iops` | 4 | 554 | IOPS | 2.5 |
| `disk_rand_write_iops` | 16 | 585 | IOPS | 3.5 |

## Verify 3: the cold check

| run | trial 0 | median of trials 1..9 | trial 0 / rest | min | max |
|---|---:|---:|---:|---:|---:|
| no warmup, cold start | 2126 | 2422 | 0.878 | 2126 | 2552 |
| after the whole file was read into the page cache | 2130 | 2377 | 0.896 | 2130 | 2540 |

Median over all ten trials: cold start 2377 MB/s, page cache deliberately filled 2375 MB/s (-0.1 %).

## Per-trial values, sequential read, no warmup (MB/s)

```
cold start : 2126  2235  2552  2453  2422  2425  2332  2254  2285  2465
warm cache : 2130  2341  2528  2351  2374  2496  2325  2540  2377  2399
```

## Verify 2c: engine vs fio, three alternating repetitions

A single A-then-B comparison confounds the tool with when it ran (M2.5's lesson), and
the Windows host's cache over the VHDX is exactly the kind of thing that drifts.

| measurement | rep | bench | fio | bench/fio |
|---|---:|---:|---:|---:|
| seq read MB/s, 1 thread(s) | 1 | 2557 | 2642 | 0.968 |
| seq read MB/s, 1 thread(s) | 2 | 2639 | 2575 | 1.025 |
| seq read MB/s, 1 thread(s) | 3 | 2481 | 2602 | 0.954 |
| seq read MB/s, 4 thread(s) | 1 | 5424 | 5532 | 0.981 |
| seq read MB/s, 4 thread(s) | 2 | 5522 | 5613 | 0.984 |
| seq read MB/s, 4 thread(s) | 3 | 5375 | 5430 | 0.990 |
| rand read IOPS, QD 1 | 1 | 8558 | 8742 | 0.979 |
| rand read IOPS, QD 1 | 2 | 7523 | 8110 | 0.928 |
| rand read IOPS, QD 1 | 3 | 8174 | 8530 | 0.958 |
| rand read IOPS, QD 4 | 1 | 31898 | 31407 | 1.016 |
| rand read IOPS, QD 4 | 2 | 29974 | 30129 | 0.995 |
| rand read IOPS, QD 4 | 3 | 31786 | 29948 | 1.061 |
| rand read IOPS, QD 16 | 1 | 91425 | 86035 | 1.063 |
| rand read IOPS, QD 16 | 2 | 83424 | 84394 | 0.989 |
| rand read IOPS, QD 16 | 3 | 79893 | 77558 | 1.030 |

| measurement | median bench/fio | range |
|---|---:|---|
| seq read MB/s, 1 thread(s) | **0.968** | 0.954-1.025 |
| seq read MB/s, 4 thread(s) | **0.984** | 0.981-0.990 |
| rand read IOPS, QD 1 | **0.958** | 0.928-0.979 |
| rand read IOPS, QD 4 | **1.016** | 0.995-1.061 |
| rand read IOPS, QD 16 | **1.030** | 0.989-1.063 |
