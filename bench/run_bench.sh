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

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
RESULTS="$ROOT/results"
RAW="$RESULTS/raw"

SERVERS=(sequential_server threaded_server threadpool_server select_server epoll_server)

mkdir -p "$RAW"
SUMMARY="$RESULTS/summary.csv"
echo "server,connections,requests,errors,mismatches,wall_ms,throughput_rps,mean_us,p50_us,p90_us,p99_us,p999_us,max_us" > "$SUMMARY"

if [[ ! -x "$BUILD/loadgen" ]]; then
  echo "loadgen not built. Run: make release" >&2
  exit 1
fi

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
    out="$("$BUILD/loadgen" 127.0.0.1 "$port" "$conns" "$MESSAGES" "$PAYLOAD" "$raw" 2>&1)"
    rc=$?

    kill "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null

    get() { echo "$out" | awk -v k="$1" '$1==k {print $2}'; }

    if [[ -z "$(get requests)" ]]; then
      echo "$server @ $conns conns: FAILED"
      echo "$out" | sed 's/^/    /'
      continue
    fi

    echo "$server,$conns,$(get requests),$(get errors),$(get mismatches),$(get wall_ms),$(get throughput_rps),$(get mean_us),$(get p50_us),$(get p90_us),$(get p99_us),$(get p999_us),$(get max_us)" >> "$SUMMARY"

    printf '%-20s %5s conns  %8s rps  p50 %8s us  p99 %9s us%s\n' \
      "$server" "$conns" "$(get throughput_rps)" "$(get p50_us)" "$(get p99_us)" \
      "$([[ $rc -ne 0 ]] && echo '  [ERRORS]' || echo '')"
  done
done

echo
echo "Summary written to $SUMMARY"
echo "Raw latencies in $RAW"
