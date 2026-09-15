# Concurrent Servers

[![build](https://github.com/RaduPetrila-dev/Concurrent-Servers/actions/workflows/ci.yml/badge.svg)](https://github.com/RaduPetrila-dev/Concurrent-Servers/actions/workflows/ci.yml)

Five concurrency models for the same TCP server, written in C against the same
protocol so they can be compared directly: one connection at a time, one thread
per connection, a bounded thread pool, `select`, and `epoll`. Plus a load
generator that measures what each one actually does under concurrency, and a
Prometheus endpoint that reports the same overload from inside the server.

The four tutorial servers follow Eli Bendersky's
[Programming concurrent servers](https://eli.thegreenplace.net/2017/concurrent-servers-part-1-introduction/)
series. The thread pool, the shared protocol module, the benchmark harness, the
defect analysis in [NOTES.md](NOTES.md), and everything in `results/` are mine.

## The protocol

Every server speaks the same thing, so the models differ only in how they wait.

1. On connect, the server sends `*`.
2. The client sends bytes. Anything outside a frame is ignored.
3. `^` opens a frame, `$` closes it.
4. Inside a frame, every byte is incremented by one and echoed back.

So `^abc$` comes back as `bcd`. The framing means a message can be split across
any number of TCP segments, which is the point: it forces the server to keep
per-connection state rather than treating each `recv` as a complete request.

## Build

```
make            # every server plus the load generator, into build/
make uv         # the libuv servers, needs libuv installed
make uring      # the io_uring server, needs liburing installed
make test       # build and run the metrics unit tests
make bench      # release build, full sweep, charts
make help       # list targets and the servers found
```

Sources are discovered from `src/`, so adding a server needs no Makefile edit.

| Target | What it does |
| --- | --- |
| `make strict` | rebuild with `-Werror`, including the libuv and io_uring servers when their libraries are present |
| `make debug` | AddressSanitizer and UndefinedBehaviorSanitizer, no optimisation |
| `make tsan` | ThreadSanitizer, which is what proves the lock-free metric counters |
| `make release` | `-O2`, the only build benchmark numbers come from |
| `make test` | metrics unit tests |
| `make test-debug` | the same tests under ASan and UBSan |
| `make test-tsan` | the same tests under ThreadSanitizer |
| `make format` | run clang-format over `src/`, `bench/` and `tests/` |
| `make format-check` | same file set, fails instead of rewriting |
| `make clean` | remove `build/` |

`test-tsan` needs ASLR entropy at 28 bits or lower, which ThreadSanitizer
normally arranges for itself through `personality(2)`. A container seccomp
profile denies that syscall, so the dev container runs with
`seccomp=unconfined`. Outside one, the target probes `setarch` and falls back to
running direct, and tells you what to change if neither route is open.

CI runs `format-check`, `strict` on gcc and clang, all three sanitizer targets,
the config checks in `tools/`, and a smoke test that drives real traffic through
`threadpool_server` and compares what the load generator saw against what the
server reported.

Opening the repo in a Codespace or dev container installs the full toolchain,
including `clang` for the second CI compiler leg, `liburing-dev` for
`uring_server`, and `shellcheck` and `tcpdump` for the observability job and the
capture script. It also runs with `NET_RAW` so `tools/capture_stall.sh` works.

## The servers

All listen on port 9090 by default, overridden by the first argument.

| Model | Source | How it waits | Where it stops |
| --- | --- | --- | --- |
| Sequential | `sequential_server.c` | blocks in `recv` | one client at a time |
| Thread per connection | `threaded_server.c` | one detached pthread per peer | thread stacks and scheduler contention |
| Thread pool | `threadpool_server.c` | N workers pull from a bounded queue | backlog + queue depth + workers, hard |
| `select` | `select_server.c` | one loop, `select` over an fd set | `FD_SETSIZE`, and O(n) scanning per call |
| `epoll` | `epoll_server.c` | one loop, level-triggered `epoll` | `MAXFDS` at 16384 |
| libuv | `uv_server.c` | libuv's event loop | abstraction over `epoll`, not a separate mechanism |
| `io_uring` | `uring_server.c` | submission and completion rings, one loop | needs `liburing`, so it sits outside the default build |

`sequential_server`, `threaded_server` and `threadpool_server` share the framing
state machine in `protocol.c`. `select_server` and `epoll_server` keep their own,
because a non-blocking handler must return before a message is necessarily
complete and so has to hold the parser state per file descriptor.

`threadpool_server` takes `[port] [workers] [queue_depth] [metrics_port]`,
defaulting to `9090 4 64 9110`. `METRICS_PORT` in the environment sets the fourth
value, and an explicit argument wins over it. Either set to 0 turns the metrics
endpoint off, which is what a benchmark sweep wants when several servers run back
to back. Its acceptor blocks when the queue fills rather than dropping the
connection, so overload shows up as refused connections instead of being hidden.

## The demonstration programs

These are not servers under test. They exist to show why the models above look
the way they do.

| Program | Shows |
| --- | --- |
| `blocking_listener.c` | a blocking `recv` parking the process until data arrives |
| `nonblocking_listener.c` | `O_NONBLOCK` returning `EAGAIN`, and the cost of polling for it |
| `threadspammer.c` | resource usage of N idle threads, run it with 10000 and watch `top` |
| `uv_timer_sleep_demo.c` | blocking inside a callback stalling the whole event loop |
| `uv_timer_work_demo.c` | the same work moved to libuv's thread pool |
| `uv_isprime_server.c` | CPU-bound work in an event loop, with `MODE=BLOCK` to compare |

`blocking_listener` and `nonblocking_listener` use port 9988 and handle exactly
one connection before exiting. Two Python thread pool servers sit in `python/`
as a comparison against the C implementations and are not part of the build.

## Trying it

```
make
./build/epoll_server 9090
```

In another terminal:

```
printf '^hello$' | nc localhost 9090
```

Expect `*` on connect, then `ifmmp`.

## Benchmarking

```
ulimit -n 8192
make bench
```

`bench/loadgen.c` opens N connections on N threads, sends M framed messages down
each, and times every round trip. It verifies each echo against the expected
transform and exits non-zero on any mismatch, so a server that is fast and wrong
fails the run rather than posting a good number.

Two details decide whether the numbers mean anything. It sets `TCP_NODELAY`,
without which Nagle batches the small writes and every latency figure measures
the 40ms delayed-ack timer instead of the server. And all connections
synchronise on a barrier before the request clock starts, so connection setup
cannot leak into throughput.

`bench/run_bench.sh` sweeps 1 to 1024 connections across every built server, each
on its own port to avoid TIME_WAIT collisions, writing a timestamped directory
under `results/`. `bench/plot_results.py` turns that into three charts.

## Metrics

The load generator sees the outside of the server. It counts what it managed to
establish and how long each round trip took, and infers the rest. The metrics
endpoint reports the same overload from inside the process, which is the only
way to tell a connection the server refused from one the kernel dropped before
`accept` ever saw it.

`threadpool_server` serves `GET /metrics` on port 9110 in the Prometheus text
exposition format. Every series carries a `model` label, so several servers
scrape into one dashboard. What the endpoint bought is in
[Client, server and kernel agree to the connection](#client-server-and-kernel-agree-to-the-connection)
below.

| Series | Type | What it answers |
| --- | --- | --- |
| `server_connections_accepted_total` | counter | how many connections reached `accept` |
| `server_connections_refused_total` | counter | how many the process turned away itself, out of descriptors |
| `server_connections_active` | gauge | how many are open right now |
| `server_pool_queue_depth` | gauge | how full the bounded queue is, the backpressure signal |
| `server_pool_workers_busy` | gauge | workers inside `serve_connection`, not workers waiting on the queue |
| `server_request_duration_seconds` | histogram | service time per request, ten buckets from 20 microseconds to 50 milliseconds |

The histogram is instrumented in `protocol.c`, so every server sharing the
framing state machine populates it. The clock starts after `recv` returns, not
before, because time spent blocked waiting on the peer is the client's think time
and not the server's. Only `threadpool_server` opens the endpoint so far, so
`sequential_server` and `threaded_server` count requests nobody scrapes.

```
docker compose -f deploy/docker-compose.yml up -d
./build/threadpool_server
```

Prometheus lands on `localhost:9091`, Grafana on `localhost:3000` with the
dashboard preloaded and anonymous viewing on. Prometheus scrapes the server at
`host.docker.internal:9110` every second, and a node-exporter alongside it for
`ListenOverflows`, `ListenDrops` and `TCPSynRetrans`. Those are the kernel's
count of connections dropped before userspace saw them, which is the other half
of the refusal story.

Three alert rules ship in `deploy/prometheus/alerts.yml`: connections being
refused, the pool queue sitting at capacity, and tail latency above target.
`tools/capture_stall.sh` takes a matching packet capture and counter delta for
the same window. [docs/observability.md](docs/observability.md) covers the
wiring, why the buckets are where they are, and the difference between the accept
queue and the SYN queue.

## Repository layout

```
src/      servers, the shared protocol module, the metrics library
bench/    load generator, sweep script, plotting
tests/    metrics unit tests, run by make test
tools/    packet capture helper, dashboard query extractor
deploy/   Prometheus, Grafana and node-exporter stack
docs/     observability write-up
results/  committed benchmark runs with the environment they came from
python/   thread pool servers for comparison, not part of the build
```

## Results

Two committed runs, both on a 2-core GitHub Codespace, Xeon Platinum 8573C,
backlog 64, 200 messages of 32 bytes per connection. Full conditions in
`results/<run>/environment.txt`.

| Run | Kernel | Servers | What it adds |
| --- | --- | --- | --- |
| `20260815-192859` | 6.8.0-1052-azure | five | the first sweep |
| `20260915-210741` | 6.8.0-1064-azure | six | `uring_server`, and the server-side counters below |

A shared container is a poor place to measure tail latency, so the shape of the
curves is the finding rather than the absolute numbers. Where the two runs
disagree, both stay recorded. The newer one does not quietly replace the older.

The September run carries the latency instrumentation in `protocol.c` and the
August one does not, so the cost of the two `CLOCK_MONOTONIC` reads per request
was measured rather than assumed: five repetitions each way at 32 connections
and 500 messages, release build, endpoint disabled. Median p50 came out at 21.7
microseconds without the instrumentation and 21.5 with it, against a run-to-run
spread of roughly 1.5 microseconds either side. The overhead sits under the noise
floor of this hardware, so the two runs stay comparable.

### Client, server and kernel agree to the connection

The load generator sees connections it failed to open. The server sees
connections it accepted. The kernel sees connections it dropped before `accept`
was ever called. Until the metrics endpoint existed only the first of those was
measurable, and the ceiling had to be inferred from the client and from reading
the code.

Ten iterations at 1024 offered connections and 2000 messages each:

| Source | Quantity | Value |
| --- | --- | --- |
| load generator | connections offered | 10,240 |
| load generator | connections established | 1,341 |
| server endpoint | `server_connections_accepted_total` | 1,341 |
| load generator | requests completed | 2,682,000 |
| server endpoint | `server_request_duration_seconds_count` | 2,682,000 |
| server endpoint | `server_connections_refused_total` | 0 |
| kernel | `ListenOverflows` delta | 44,498 |
| kernel | `TCPSynRetrans` delta | 35,599 |

Both server counters match the client exactly. The refusal counter sits at zero,
which places every one of the 8,899 lost connections in the kernel rather than in
userspace. The process turned nobody away. It never saw them.

The kernel deltas then close the loop:

```
ListenOverflows 44,498  -  TCPSynRetrans 35,599  =  8,899
```

That difference is the shortfall, to the connection. One overflow for the
original SYN, one more for each of the four retransmits inside the load
generator's five second connect timeout. Three counters, two of them kernel-side
and outside this process entirely, reconciling with no fitting.

The client's own arithmetic holds alongside it. Every iteration reports
established times 2000 as requests and the remainder times 2000 as errors, so no
connection that established ever dropped a message.

### Refusing load looks like winning

The thread pool established 134 connections when offered 256, 512 and 1024, and
135 on one run. That is `N_BACKLOG` 64, plus queue depth 64, plus 4 workers, with
Linux rounding the backlog up. The figure is stable across both sweeps and across
ten repetitions of the overload run.

At 1024 offered it turns away 87% of clients and posts the highest throughput and
the flattest median latency on the chart: 108,281 requests per second at a 28.9
microsecond p50 in the September run, against real servers serving every
connection at ten times the latency. `established.png` exists because of this. It
plots connections served against connections offered with the ideal diagonal, and
the thread pool flatlines while everything else follows the line. Points with
failed requests are ringed on every chart for the same reason.

`connect_max_us` separates the two ways a connection is lost. For `threaded`,
`select`, `epoll` and `uring` it lands near 1.05 seconds, so those connections
got in on the first SYN retransmit. For the thread pool at 256 and above it hits
the load generator's five second ceiling, because past its hard limit the
connection never gets in at all.

### Where the thread pool is genuinely ahead, it is far ahead

At 128 connections, serving all of them with no errors, it ran at a p50 of 27.5
microseconds and a p99 of 131, against thread-per-connection at 1,196 and 3,814.
Four workers on two cores beat 128 threads by **43x** on median latency at
comparable throughput, 120,415 against 123,996 requests per second, because
bounded concurrency avoids the scheduler contention a thread per connection
creates.

The August run put the same comparison at 27 against 1,069 microseconds, so the
effect reproduced on a newer kernel and two newer compiler generations, slightly
larger the second time.

### An overflowing accept backlog costs exactly one second

Once offered connections exceed the listen backlog, `connect_max_us` lands
between 1.02 and 1.08 seconds for `threaded`, `select`, `epoll` and `uring`
alike, in both runs. That is Linux's initial SYN retransmit timeout, and four
unrelated serving architectures landing on the same figure is what identifies it
as a kernel constant rather than a property of any of them.

Before the harness separated setup from service this appeared as a throughput
collapse at 128 connections across three unrelated architectures, which is what
gave it away: no property of a serving model produces the same cliff in
thread-per-connection, `select` and `epoll` within 6% of each other.

### `select` against `epoll`: a finding that did not reproduce

In August, `epoll` served all 256 connections in 702ms with no errors while
`select` took 10.2 seconds and failed 5,600 requests, which is the O(n) cost of
rebuilding and walking the fd set on every call.

In September both failed at 256, at 4,391 and 4,907 requests per second, and the
gap between them disappeared. Nothing in this repo changed either server. They
keep their own state machines and the `protocol.c` instrumentation never reaches
them, so the cause sits in the kernel version, in container noise, or in an
August run that got lucky.

The claim is therefore open rather than established, and it stays here in that
form. Explaining the divergence is on the list below.

### `io_uring`, first measurement

`uring_server` entered the sweep for the first time in September. One run on a
shared container, so read it as a direction rather than a result.

| Connections | `epoll` rps | `epoll` p50 | `uring` rps | `uring` p50 |
| --- | --- | --- | --- | --- |
| 1 | 20,650 | 48.8 us | 46,017 | 14.3 us |
| 32 | 109,654 | 246.1 us | 147,353 | 202.2 us |
| 128 | 97,663 | 1,212 us | 112,212 | 1,070 us |
| 256 | 4,907 | 2,409 us | 99,559 | 2,171 us |

At one connection `uring` runs at a third of `epoll`'s median, which is the
syscall batching showing up where there is nothing else to hide it. At 256 it was
the only server in the sweep to serve every offered connection with no failed
requests, while holding 99,559 requests per second where `epoll` fell to 4,907.

That 256-connection column is also where `select` and `epoll` both diverged from
August, so treat the two as one open question rather than two separate results.

## Known issues

[NOTES.md](NOTES.md) tracks every defect found in the code and in the benchmark
harness, tagged by whether it breaks under real use, costs measurable
performance, is a maintainability problem, or is a limitation of the model that
should be kept and demonstrated.

Two are worth naming here because they shape how the code should be read.

**Bounds checking inside `assert`.** `select_server.c` and `epoll_server.c` guard
a buffer write with `assert(sendbuf_end < SENDBUF_SIZE)`. The tutorial's Makefile
passes `-DNDEBUG`, which expands `assert` to nothing, so the guard is absent from
the build that ships. This repo builds without `NDEBUG` by default and keeps it
for the `release` target only. That change alone surfaced an undeclared
identifier in `select_server.c` that had been invisible.

**`EPOLLERR` terminates the process.** `epoll_server.c` calls `perror_die` on the
error flag, which a peer sending RST raises routinely. `protocol.c` fixed this
for the blocking servers by returning a per-connection result instead of exiting.
The event-driven pair have not been converted yet.

## Planned

- Explain the 256-connection divergence between the two runs. `select` and `epoll` lost the gap that separated them in August, and `uring` was the only server to serve all 256 cleanly. One cause probably covers both
- Convert `select_server.c` and `epoll_server.c` onto a shared non-blocking parser
- Raise `N_BACKLOG` from 64 and re-measure. The thread pool ceiling should move by exactly the amount added, which is a prediction from the code testable by measurement
- Open the metrics endpoint on the remaining servers, so the dashboard compares models rather than reporting one
- Repeat the sweeps on dedicated hardware, to separate model behaviour from container noise

## Credit

Server implementations from Eli Bendersky's
[Programming concurrent servers](https://eli.thegreenplace.net/2017/concurrent-servers-part-1-introduction/),
released into the public domain. Everything else here is mine, under MIT.
