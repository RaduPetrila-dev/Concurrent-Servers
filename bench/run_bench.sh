#!/usr/bin/env bash
# Sweeps connection counts across every server and records the results.
#
# Usage: bench/run_bench.sh [messages_per_conn] [payload_bytes]
#
# Writes results/summary.csv with one row per (server, connections) pair, and
# results/raw/<server>_<connections>.csv with every individual latency.
#
# Servers are started and stopped by this script. Each runs on its own port to
# avoid TIME_WAIT collisions between runs.

set -uo pipefail

MESSAGES="${1:-200}"
PAYLOAD="${2:-32}"
CONN_COUNTS=(1 2 4 8 16 32 64 128 256 512 1024)

# Hard ceiling per data point. loadgen has its own per-syscall timeouts; this is
# the backstop for anything they miss, so one saturated point cannot stall the
# whole sweep.
RUN_TIMEOUT="${RUN_TIMEOUT:-180}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"

# One directory per run, so a second sweep never overwrites the first. The
# comparison between runs is part of the result.
RUN_ID="${RUN_ID:-$(date +%Y%m%d-%H%M%S)}"
RESULTS="$ROOT/results/$RUN_ID"
RAW="$RESULTS/raw"

SERVERS=(sequential_server threaded_server threadpool_server select_server epoll_server)

mkdir -p "$RAW"
SUMMARY="$RESULTS/summary.csv"
echo "server,connections,established,setup_ms,connect_max_us,ack_max_us,requests,errors,mismatches,wall_ms,throughput_rps,mean_us,p50_us,p90_us,p99_us,p999_us,max_us" > "$SUMMARY"

if [[ ! -x "$BUILD/loadgen" ]]; then
  echo "loadgen not built. Run: make release" >&2
  exit 1
fi

# Each connection needs an fd on both sides, and the default soft limit is
# usually 1024, which the top of the sweep exceeds.
soft_limit="$(ulimit -n)"
needed=$(( ${CONN_COUNTS[-1]} * 2 + 64 ))
if [[ "$soft_limit" != "unlimited" && "$soft_limit" -lt "$needed" ]]; then
  ulimit -n "$needed" 2>/dev/null || true
  soft_limit="$(ulimit -n)"
  if [[ "$soft_limit" -lt "$needed" ]]; then
    echo "warning: fd limit is $soft_limit, want $needed. High connection"
    echo "         counts will report errors. Raise it with: ulimit -n $needed"
  fi
fi

{
  echo "date         $(date -Iseconds)"
  echo "kernel       $(uname -sr)"
  echo "cores        $(nproc)"
  echo "cpu          $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
  echo "cgroup_cpu   $(cat /sys/fs/cgroup/cpu.max 2>/dev/null || echo unknown)"
  echo "memory       $(free -h | awk '/^Mem:/ {print $2}')"
  echo "ulimit_n     $(ulimit -n)"
  echo "messages     $MESSAGES"
  echo "payload      $PAYLOAD"
} > "$RESULTS/environment.txt"
cat "$RESULTS/environment.txt"
echo

port=9200
for server in "${SERVERS[@]}"; do
  if [[ ! -x "$BUILD/$server" ]]; then
    echo "skipping $server (not built)"
    continue
  fi

  for conns in "${CONN_COUNTS[@]}"; do
    # The sequential server serves one client at a time; anything above a
    # handful of connections takes minutes and tells us nothing new.
    if [[ "$server" == "sequential_server" && "$conns" -gt 8 ]]; then
      continue
    fi

    port=$((port + 1))
    "$BUILD/$server" "$port" > /dev/null 2>&1 &
    server_pid=$!
    sleep 0.4

    raw="$RAW/${server}_${conns}.csv"
    out="$(timeout "$RUN_TIMEOUT" "$BUILD/loadgen" 127.0.0.1 "$port" "$conns" \
             "$MESSAGES" "$PAYLOAD" "$raw" 2>&1)"
    rc=$?

    kill "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null

    get() { echo "$out" | awk -v k="$1" '$1==k {print $2}'; }

    if [[ $rc -eq 124 ]]; then
      echo "$server @ $conns conns: TIMED OUT after ${RUN_TIMEOUT}s, skipping rest of this server"
      break
    fi

    if [[ -z "$(get requests)" ]]; then
      echo "$server @ $conns conns: FAILED"
      echo "$out" | sed 's/^/    /'
      continue
    fi

    echo "$server,$conns,$(get established),$(get setup_ms),$(get connect_max_us),$(get ack_max_us),$(get requests),$(get errors),$(get mismatches),$(get wall_ms),$(get throughput_rps),$(get mean_us),$(get p50_us),$(get p90_us),$(get p99_us),$(get p999_us),$(get max_us)" >> "$SUMMARY"

    printf '%-20s %5s conns  %8s rps  p50 %8s us  p99 %9s us  connect_max %9s us%s\n' \
      "$server" "$conns" "$(get throughput_rps)" "$(get p50_us)" "$(get p99_us)" \
      "$(get connect_max_us)" \
      "$([[ $rc -ne 0 ]] && echo '  [ERRORS]' || echo '')"
  done
done

# Convenience pointer for `make bench` and for plotting the newest run.
ln -sfn "$RUN_ID" "$ROOT/results/latest"

echo
echo "Summary written to $SUMMARY"
echo "Raw latencies in $RAW"
echo "results/latest now points at $RUN_ID"
