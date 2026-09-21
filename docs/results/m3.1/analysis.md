## Working-set sweep, 1 thread (GB/s, median of 10 x 50 ms)

| working set | read | write | copy | write/read |
|---|---:|---:|---:|---:|
| 8 KiB | 213.8 | 237.7 | 296.8 | 1.11 |
| 16 KiB | 237.8 | 273.0 | 302.6 | 1.15 |
| 32 KiB | 253.7 | 290.9 | 67.6 | 1.15 |
| 64 KiB | 190.6 | 80.6 | 68.9 | 0.42 |
| 128 KiB | 201.7 | 80.0 | 65.3 | 0.40 |
| 256 KiB | 201.8 | 79.8 | 69.0 | 0.40 |
| 512 KiB | 201.0 | 80.6 | 65.0 | 0.40 |
| 1 MiB | 173.4 | 75.4 | 36.3 | 0.43 |
| 2 MiB | 181.8 | 78.6 | 37.7 | 0.43 |
| 4 MiB | 72.7 | 50.4 | 35.9 | 0.69 |
| 8 MiB | 72.2 | 50.3 | 36.5 | 0.70 |
| 16 MiB | 45.0 | 39.6 | 31.7 | 0.88 |
| 32 MiB | 23.9 | 20.4 | 25.5 | 0.85 |
| 64 MiB | 26.0 | 15.1 | 23.0 | 0.58 |
| 256 MiB | 22.9 | 14.1 | 21.4 | 0.61 |

## Thread sweep at 256 MiB/thread, mem_read_bw

| threads | GB/s | GB/s per thread | vs 1 thread | CoV % | late % |
|---:|---:|---:|---:|---:|---:|
| 1 | 23.1 | 23.10 | 1.00x | 1.9 | 0 |
| 2 | 38.0 | 18.99 | 1.64x | 2.8 | 0 |
| 3 | 42.4 | 14.13 | 1.84x | 3.2 | 0 |
| 4 | 48.5 | 12.14 | 2.10x | 9.3 | 0 |
| 6 | 68.4 | 11.40 | 2.96x | 3.2 | 0 |
| 8 | 70.8 | 8.86 | 3.07x | 2.7 | 0 |
| 11 | 79.8 | 7.25 | 3.45x | 2.6 | 0 |
| 16 | 84.2 | 5.26 | 3.65x | 2.3 | 0 |
| 22 | 64.0 | 2.91 | 2.77x | 14.9 | 100 |

## DRAM regime (256 MiB/thread): plain vs non-temporal stores

| threads | stores | read | write | copy | write/read |
|---:|---|---:|---:|---:|---:|
| 1 | plain | 21.0 | 13.2 | 21.4 | 0.63 |
| 1 | non-temporal | 23.1 | 43.5 | 19.1 | 1.88 |
| 8 | plain | 67.9 | 41.4 | 39.7 | 0.61 |
| 8 | non-temporal | 66.1 | 59.5 | 41.9 | 0.90 |

## Does glibc memcpy already use non-temporal stores?

| working set | default threshold (24 MiB) | threshold forced to 1 GiB | change |
|---|---:|---:|---:|
| 8 MiB | 35.49 | 36.48 | +2.8 % |
| 256 MiB | 21.31 | 18.71 | -12.2 % |
