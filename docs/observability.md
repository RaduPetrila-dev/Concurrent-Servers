# Observability

Live telemetry for the servers in `src/`, plus the kernel-side evidence for the
connect stall seen once the accept backlog overflows.

## What each layer sees

The server process and the kernel disagree about how many connections were
offered, and the gap is the point of this project.

| Question | Where the answer lives |
| --- | --- |
| How many connections reached `accept()`? | `server_connections_accepted_total` |
| How many did the server itself turn away? | `server_connections_refused_total` |
| How many were dropped before `accept()` saw them? | `node_netstat_TcpExt_ListenOverflows` |
| How many peers retransmitted a SYN? | `node_netstat_TcpExt_TCPSynRetrans` |

A connection killed by accept-backlog overflow never reaches userspace, so the
server-side refusal counter undercounts. The load generator's view of offered
connections and the kernel's drop counters are the only honest denominators.

## Wiring the instrumentation in

Add to `src/threadpool_server.c` (the same six hooks apply to any other model).

```c
#include "metrics.h"
#include "metrics_server.h"
```

In `main()`, after the listening socket is up:

```c
metrics_init("threadpool");
if (metrics_server_start(9110) != 0) {
    perror("metrics_server_start");
    return 1;
}
```

Call `metrics_server_stop()` on the shutdown path, or register it once with
`atexit(metrics_server_stop)`.

In the accept loop, on a successful `accept()`:

```c
metrics_connection_accepted();
```

Where a full queue means the connection is closed without being served:

```c
metrics_connection_refused();
```

On per-connection teardown, exactly once per accepted connection:

```c
metrics_connection_closed();
```

On enqueue and dequeue, while the queue mutex is already held:

```c
metrics_queue_depth_set(queue->count);
```

Around request service inside the worker:

```c
uint64_t started = metrics_now_ns();
metrics_workers_busy_add(1);
serve_connection(fd);
metrics_workers_busy_add(-1);
metrics_request_observed(metrics_now_ns() - started);
```

`metrics_init` accepts `[A-Za-z0-9_-]` only. Anything else falls back to
`unknown`, so a bad label never produces malformed exposition output.

## Histogram buckets

Bucket boundaries live in `k_bucket_upper_ns` in `src/metrics.c` and span 20µs
to 50ms. Percentiles are interpolated within a bucket, so resolution is bounded
by the boundaries, not by the sample count. The thread pool's 29µs median sits
in the second bucket and thread-per-connection's 4.8ms p99 in the eighth.
Changing the concurrency model or the hardware means revisiting these.

Boundaries are inclusive: a request of exactly 20000ns lands in `le="0.00002"`.

## Build

`metrics.c` and `metrics_server.c` have no `main`, so they must join
`COMMON_SRCS`. Leave them out and the generic binary rule tries to link them as
standalone servers and fails with `undefined reference to 'main'`.

```make
COMMON_SRCS := $(SRC_DIR)/utils.c $(SRC_DIR)/protocol.c $(SRC_DIR)/peer_state.c \
               $(SRC_DIR)/metrics.c $(SRC_DIR)/metrics_server.c
```

Adding them there does two things at once: every server links against the
metrics objects, and `filter-out $(COMMON_SRCS)` keeps them out of `ALL_SRCS`
so no stray `build/metrics` binary is attempted.

The test target links `metrics.c` directly rather than the object, so a
sanitiser run of the tests stays independent of whatever flags the servers were
built with.

```make
$(BUILD_DIR)/test_metrics: $(TEST_DIR)/test_metrics.c $(SRC_DIR)/metrics.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(SRC_DIR) $^ $(LDFLAGS) -o $@

.PHONY: test
test: $(TEST_BINS)
	$(BUILD_DIR)/test_metrics
```

## Run

Binary targets live under `build/`, so name the path rather than the server.

```sh
make release
./build/threadpool_server &

cd deploy && docker compose up -d
```

- Grafana: http://localhost:3000, dashboard provisioned on start
- Prometheus: http://localhost:9091

Port choices matter here. The servers listen on 9090, which is also
Prometheus's own default, so the container publishes 9091 on the host. The
metrics endpoint uses 9110 rather than 9100 because 9100 belongs to
node_exporter by convention.

`host.docker.internal` resolves inside the container through the
`host-gateway` entry in `docker-compose.yml`. On Docker Desktop it resolves
without that entry; on Linux it does not.

## Capturing the connect stall

```sh
tools/capture_stall.sh -- ./build/loadgen 127.0.0.1 9090 1024 200
```

Writes `artefacts/stall-<timestamp>/` containing the sysctl values in force,
`ss -lntie` before and after, a counter delta table, and a pcap of every SYN,
RST and FIN on the port.

Read the counter table first. It settles the mechanism on its own:

- `ListenOverflows` rising means the accept queue filled. With
  `tcp_abort_on_overflow=0` the kernel drops the client's final ACK and the
  server retransmits its SYN-ACK.
- `TCPReqQFullDrop` or `SyncookiesSent` rising means the SYN queue filled,
  sized by `tcp_max_syn_backlog` rather than by the `listen()` backlog. Here
  the SYN itself is dropped and the client retransmits.

Both produce a delay near one second, and they are different bugs.

Wireshark display filters:

```
tcp.analysis.retransmission and tcp.flags.syn == 1 and tcp.flags.ack == 0   # client SYN retransmit
tcp.analysis.retransmission and tcp.flags.syn == 1 and tcp.flags.ack == 1   # server SYN-ACK retransmit
```

Set View, Time Display Format, Seconds Since Previous Displayed Packet. Linux
starts the SYN RTO at one second and doubles it, so retransmits land near
t=1s, t=3s and t=7s. Gaps that do not sit on that ladder mean the delay is
somewhere other than the handshake.

Inside a container `tcpdump` needs `CAP_NET_RAW`. Run the devcontainer with
`--cap-add=NET_RAW`, or put server and load generator on a Docker bridge
network and capture the bridge from the host, which exercises a real interface
rather than loopback.
