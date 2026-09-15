#!/usr/bin/env bash
#
# Records the kernel-side evidence for the connect stall seen at high
# connection counts, alongside a packet trace of the TCP handshakes.
#
#   tools/capture_stall.sh -- ./bench/loadgen 127.0.0.1 9090 1024 200
#
# Everything lands in artefacts/stall-<timestamp>/.

set -euo pipefail

PORT="${PORT:-9090}"
IFACE="${IFACE:-lo}"
OUT_ROOT="${OUT_ROOT:-artefacts}"

usage() {
    cat >&2 <<'EOF'
usage: capture_stall.sh [-p port] [-i interface] -- <command to run under capture>

  -p, --port       server port to filter on (default 9090)
  -i, --interface  interface to capture on (default lo)

environment:
  OUT_ROOT  directory for artefacts (default ./artefacts)
EOF
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port) PORT="$2"; shift 2 ;;
        -i|--interface) IFACE="$2"; shift 2 ;;
        -h|--help) usage ;;
        --) shift; break ;;
        *) usage ;;
    esac
done

[[ $# -gt 0 ]] || usage

require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing required tool: $1" >&2
        exit 1
    }
}

require tcpdump
require ss
require nstat
require sysctl

OUT_DIR="${OUT_ROOT}/stall-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "${OUT_DIR}"

COUNTERS='ListenOverflows|ListenDrops|TCPReqQFullDrop|TCPReqQFullDoCookies|SyncookiesSent|TCPSynRetrans'

snapshot_counters() {
    nstat -az 2>/dev/null | awk -v pat="${COUNTERS}" '$1 ~ pat { print $1, $2 }' | sort
}

TCPDUMP_PID=""
cleanup() {
    if [[ -n "${TCPDUMP_PID}" ]] && kill -0 "${TCPDUMP_PID}" 2>/dev/null; then
        kill -INT "${TCPDUMP_PID}" 2>/dev/null || true
        wait "${TCPDUMP_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

sysctl -n net.ipv4.tcp_max_syn_backlog net.ipv4.tcp_syncookies \
    net.ipv4.tcp_abort_on_overflow net.ipv4.tcp_syn_retries \
    net.core.somaxconn > "${OUT_DIR}/sysctl.txt" 2>&1 || true

ss -lntie "sport = :${PORT}" > "${OUT_DIR}/listen_before.txt" 2>&1 || true
snapshot_counters > "${OUT_DIR}/counters_before.txt"

tcpdump -i "${IFACE}" -s 96 -w "${OUT_DIR}/handshakes.pcap" \
    "tcp port ${PORT} and (tcp[tcpflags] & (tcp-syn|tcp-rst|tcp-fin) != 0)" \
    > "${OUT_DIR}/tcpdump.log" 2>&1 &
TCPDUMP_PID=$!

sleep 1
if ! kill -0 "${TCPDUMP_PID}" 2>/dev/null; then
    echo "tcpdump exited immediately, see ${OUT_DIR}/tcpdump.log" >&2
    echo "inside a container you likely need CAP_NET_RAW" >&2
    exit 1
fi

echo "capturing on ${IFACE}, port ${PORT}"
set +e
"$@" > "${OUT_DIR}/workload.log" 2>&1
WORKLOAD_RC=$?
set -e

ss -lntie "sport = :${PORT}" > "${OUT_DIR}/listen_after.txt" 2>&1 || true
snapshot_counters > "${OUT_DIR}/counters_after.txt"

join -a1 -a2 -e 0 -o 0,1.2,2.2 \
    "${OUT_DIR}/counters_before.txt" "${OUT_DIR}/counters_after.txt" \
    | awk 'BEGIN { printf "%-28s %12s %12s %12s\n", "counter", "before", "after", "delta" }
           { printf "%-28s %12s %12s %12s\n", $1, $2, $3, $3 - $2 }' \
    > "${OUT_DIR}/counters_delta.txt"

cleanup
TCPDUMP_PID=""

{
    echo "workload exit code: ${WORKLOAD_RC}"
    echo "command: $*"
} > "${OUT_DIR}/run.txt"

echo
cat "${OUT_DIR}/counters_delta.txt"
echo
echo "artefacts in ${OUT_DIR}"
echo
echo "next:"
echo "  wireshark ${OUT_DIR}/handshakes.pcap"
echo "  display filter for client SYN retransmits:"
echo "    tcp.analysis.retransmission and tcp.flags.syn == 1 and tcp.flags.ack == 0"
echo "  display filter for server SYN-ACK retransmits:"
echo "    tcp.analysis.retransmission and tcp.flags.syn == 1 and tcp.flags.ack == 1"

exit "${WORKLOAD_RC}"
