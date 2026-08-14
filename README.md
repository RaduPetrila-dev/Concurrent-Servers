# Concurrent Servers

[![build](https://github.com/RaduPetrila-dev/Concurrent-Servers/actions/workflows/ci.yml/badge.svg)](https://github.com/RaduPetrila-dev/Concurrent-Servers/actions/workflows/ci.yml)

Four concurrency models for the same TCP server, written in C against the same
protocol so they can be compared directly: one connection at a time, one thread
per connection, `select`, and `epoll`. Plus the demonstration programs that
motivate each step.

The servers follow Eli Bendersky's
[Programming concurrent servers](https://eli.thegreenplace.net/2017/concurrent-servers-part-1-introduction/)
series. The build system, tooling, defect analysis in [NOTES.md](NOTES.md), and
the benchmarking work are mine.

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
make            # every server except the libuv ones, into build/
make uv         # the libuv servers, needs libuv installed
make everything # both
make help       # list targets and the servers found
```

Sources are discovered from `src/`, so adding a server needs no Makefile edit.

| Target | What it does |
| --- | --- |
| `make strict` | rebuild with `-Werror`, what CI runs |
| `make debug` | AddressSanitizer and UndefinedBehaviorSanitizer, no optimisation |
| `make release` | `-O2`, the only build benchmark numbers come from |
| `make format` | run clang-format over `src/` |
| `make clean` | remove `build/` |

Opening the repo in a Codespace or dev container installs `build-essential`,
`clang-format` and `libuv1-dev` automatically.

## The servers

All four listen on port 9090 by default, overridden by the first argument.

| Model | Source | How it waits | Where it stops scaling |
| --- | --- | --- | --- |
| Sequential | `sequential_server.c` | blocks in `recv` | one client at a time, a slow peer blocks everyone |
| Thread per connection | `threaded_server.c` | one detached pthread per peer | thread stacks, roughly 8MB of virtual address space each |
| `select` | `select_server.c` | one loop, `select` over an fd set | `FD_SETSIZE`, 1024 on Linux, and O(n) scanning per call |
| `epoll` | `epoll_server.c` | one loop, level-triggered `epoll` | `MAXFDS` at 16384, and a 16MB state array allocated up front |
| libuv | `uv_server.c` | libuv's event loop | abstraction over `epoll`, not a separate mechanism |

The `select` and `epoll` servers are non-blocking and keep a `peer_state_t` per
file descriptor holding the parser state and a send buffer, because a callback
returns before a message is necessarily complete.

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
one connection before exiting.

Two Python thread pool servers sit in `python/` as a comparison against the C
implementations. They are not part of the build.

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

## Known issues

[NOTES.md](NOTES.md) tracks every defect found while working through the code,
tagged by whether it breaks under real use, costs measurable performance, is a
maintainability problem, or is a limitation of the model that should be kept and
demonstrated.

Two are worth naming here because they shape how the code should be read.

**Bounds checking inside `assert`.** Both `select_server.c` and `epoll_server.c`
guard a buffer write with `assert(sendbuf_end < SENDBUF_SIZE)`. The tutorial's
Makefile passes `-DNDEBUG`, which expands `assert` to nothing, so the guard is
absent from the build that ships. This repo builds without `NDEBUG` by default
and keeps it for the `release` target only. That change alone surfaced an
undeclared identifier in `select_server.c` that had been invisible.

**One misbehaving peer terminates the process.** Every server calls `perror_die`
on a `recv` or `send` failure. In `epoll_server.c` it also fires on `EPOLLERR`,
which a peer sending RST triggers routinely. Per-connection teardown is the
correct policy and is not yet implemented.

Both are recorded rather than patched, so the four models stay comparable until
the benchmarks are in.

## Planned

- A load generator opening N concurrent connections and recording per-request latency
- Throughput and p50, p90, p99 latency across increasing connection counts
- The crossover point where each model stops scaling, and the reason in each case
- A thread pool server in C, the model that removes the thread-per-connection ceiling
- Possibly an `io_uring` implementation benchmarked against `epoll`

No numbers are published yet. When they are, the commands that produce them will
be in the repo alongside the results.

## Credit

Server implementations from Eli Bendersky's
[Programming concurrent servers](https://eli.thegreenplace.net/2017/concurrent-servers-part-1-introduction/),
released into the public domain. Everything else here is mine, under MIT.
