# Concurrent Servers

[![build](https://github.com/RaduPetrila-dev/Concurrent-Servers/actions/workflows/ci.yml/badge.svg)](https://github.com/RaduPetrila-dev/Concurrent-Servers/actions/workflows/ci.yml)

Five concurrency models for the same TCP server, written in C against the same
protocol so they can be compared directly: one connection at a time, one thread
per connection, a bounded thread pool, `select`, and `epoll`. Plus a load
generator that measures what each one actually does under concurrency.

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
make bench      # release build, full sweep, charts
make help       # list targets and the servers found
```

Sources are discovered from `src/`, so adding a server needs no Makefile edit.

| Target | What it does |
| --- | --- |
| `make strict` | rebuild with `-Werror`, what CI runs |
| `make debug` | AddressSanitizer and UndefinedBehaviorSanitizer, no optimisation |
| `make release` | `-O2`, the only build benchmark numbers come from |
| `make format` | run clang-format over `src/` and `bench/` |
| `make clean` | remove `build/` |

Opening the repo in a Codespace or dev container installs `build-essential`,
`clang-format`, `libuv1-dev`, `python3-matplotlib` and `netcat-openbsd`.

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

`sequential_server`, `threaded_server` and `threadpool_server` share the framing
state machine in `protocol.c`. `select_server` and `epoll_server` keep their own,
because a non-blocking handler must return before a message is necessarily
complete and so has to hold the parser state per file descriptor.

`threadpool_server` takes `[port] [workers] [queue_depth]`, defaulting to
`9090 4 64`. Its acceptor blocks when the queue fills rather than dropping the
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

## Results

Measured on a 2-core GitHub Codespace, Xeon Platinum 8573C, backlog 64, 200
messages of 32 bytes per connection. Full conditions in
`results/<run>/environment.txt`.

A shared container is a poor place to measure tail latency, so the shape of the
curves is the finding here rather than the absolute numbers.

### Refusing load looks like winning

The thread pool established exactly 134 connections when offered 256, 512 and
1024. That is `N_BACKLOG` 64, plus queue depth 64, plus 4 workers, with Linux
rounding the backlog up.

At 1024 offered it turned away 87% of clients, failed 178,000 requests, and
posted the highest throughput and the flattest median latency on the chart.
`established.png` exists because of this. It plots connections served against
connections offered with the ideal diagonal, and the thread pool flatlines while
everything else follows the line. Points with failed requests are ringed on every
chart for the same reason.

### `select` and `epoll` are indistinguishable until they are not

Below 128 connections their throughput and latency track each other closely. At
256 connections `epoll` served all of them in 702ms with no errors, while
`select` took 10.2 seconds and failed 5,600 requests. That is the O(n) cost of
rebuilding and walking the fd set on every call.

### An overflowing accept backlog costs exactly one second

Once offered connections exceed the listen backlog, `connect_max_us` lands
between 1.02 and 1.07 seconds for `threaded`, `select` and `epoll` alike. That is
Linux's initial SYN retransmit timeout. At 1024 connections `threaded_server`
reaches 2.08 seconds, the first retransmit plus the second.

Before the harness separated setup from service this appeared as a throughput
collapse at 128 connections across three unrelated architectures, which is what
gave it away: no property of a serving model produces the same cliff in
thread-per-connection, `select` and `epoll` within 6% of each other.

### Where the thread pool is genuinely ahead, it is far ahead

At 128 connections, serving all of them with no errors, it ran at a p50 of 27
microseconds and a p99 of 71, against thread-per-connection at 1,069 and 2,657.
Four workers on two cores beat 128 threads by roughly 40x on median latency at
comparable throughput, because bounded concurrency avoids the scheduler
contention that a thread per connection creates.

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

- Convert `select_server.c` and `epoll_server.c` onto a shared non-blocking parser
- Raise `N_BACKLOG` from 64 and re-measure, to confirm the thread pool ceiling moves by exactly the amount added
- Repeat the sweeps on dedicated hardware, to separate model behaviour from container noise
- An `io_uring` implementation benchmarked against `epoll`

## Credit

Server implementations from Eli Bendersky's
[Programming concurrent servers](https://eli.thegreenplace.net/2017/concurrent-servers-part-1-introduction/),
released into the public domain. Everything else here is mine, under MIT.
