#!/usr/bin/env bash
# Does the clock-canary re-run rule actually fire? Swept across load levels.
#
# In every study run the canary read ~14 ns and no configuration was ever re-run. That is
# the right outcome on a quiet machine, but it is not evidence the rule works. Here the
# host is deliberately oversubscribed and the sweep shows where the rule PLAN.md specifies
# (mean steady_clock::now() > 100 ns) starts to fire.
set -u
cd "$(dirname "$0")/../../.."
BIN=engine/build/release/bench
D=docs/results/m2.5
CPUS=$(nproc)

printf '%-22s %10s %10s %10s %9s\n' "load" "clk_start" "clk_end" "attempts" "verdict"
for mult in 0 1 3 10; do
  HOGS=""
  if [ "$mult" -gt 0 ]; then
    for _ in $(seq $((CPUS*mult))); do (while :; do :; done) & done
    HOGS=$(jobs -p)
    sleep 2
  fi
  $BIN run -w cpu_int -t 1 -n 5 --trial-ms 20 --warmup 2 --warmup-ms 0 --spin-ms 0 \
       --canary-retries 2 --out "$D/canary_load_$mult.json" >/dev/null 2>&1
  [ -n "$HOGS" ] && { kill $HOGS 2>/dev/null; wait 2>/dev/null; }
  python3 - "$D/canary_load_$mult.json" "$((CPUS*mult)) spinners / $CPUS vCPUs" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))["summary"][0]
a = s["canary_attempts"]
print(f'{sys.argv[2]:<22} {s["clock_call_ns_start"]:9.1f}n {s["clock_call_ns_end"]:9.1f}n '
      f'{a:10d} {"RE-RUN" if a > 1 else "kept":>9}')
PY
done
echo
echo "The rule is pre-declared: it reads only the clock, never a trial value, and it keeps"
echo "or discards a whole configuration. It is not outlier trimming."
