# Notes

Every defect found while working through this code, and what was done about it.
Includes the bugs I put in my own benchmark harness, because two of them changed
what the results appeared to say.

Legend: **[bug]** breaks under real use, **[perf]** costs measurable throughput
or latency, **[style]** correctness-adjacent or maintainability, **[design]**
inherent to the model, kept and demonstrated rather than fixed.

---

## Fixed

All of these were duplicated across `sequential_server.c` and
`threaded_server.c`, byte for byte, including the bugs. They now live once in
`protocol.c`.

**[style] The framing state machine existed in two copies.**
Every fix had to be applied twice, and both copies drifted. `protocol.c` and
`protocol.h` now hold it, and both blocking servers call `serve_connection`.
`threadpool_server.c` was written against the shared version from the start.

**[bug] SIGPIPE terminated the process.**
Writing to a socket whose peer has disconnected raises SIGPIPE, and the default
action kills the process before the `send` error branch is ever reached.
`ignore_sigpipe()` is called at startup in every server.

**[bug] EINTR was treated as fatal.**
`recv` and `send` return -1 with `errno == EINTR` when a signal arrives during
the call. Both now retry.

**[bug] One misbehaving peer took down every connection.**
`perror_die` on any I/O failure meant a single client could end the process.
`serve_connection` now returns `SERVE_OK`, `SERVE_PEER_RESET` or
`SERVE_IO_ERROR`, and the caller closes that connection and carries on.

**[bug] Partial sends silently truncated.**
The old `send(...) < 1` check only worked because exactly one byte was sent.
`send_all` loops until every byte is written.

**[perf] One `send` syscall per byte.**
A context switch per character. Bytes are now transformed in place and sent once
per `recv`. Reusing the receive buffer is safe because the output can never be
longer than the input, which also removes the bounds check the tutorial guarded
with an assert.

**[style] `int` used for `recv` and `send` return values.**
`ssize_t` throughout `protocol.c`.

**[bug] `pthread_create` return value ignored in `threaded_server.c`.**
On failure the config allocation leaked and the socket was never closed. It now
frees, closes, logs, and drops that peer rather than the whole server.

**[bug] `MAXFDs` did not exist in `select_server.c`.**
The macro is `MAXFDS`; three asserts spelled it with a lowercase s. `gcc -Wall`
rejects it, `gcc -DNDEBUG` compiles it clean, because `assert` expands to nothing
and the identifier is never evaluated. The tutorial Makefile passes `-DNDEBUG`,
which is why it survived. This repo only defines `NDEBUG` for `make release`.

**[bug] `case IN_MSG;` in `threaded_server.c`.**
Semicolon instead of a colon, which also made `-Wswitch` report the enum value as
unhandled. Both errors went away together.

**[bug] `make strict` reported success after a failed sub-build.**
The recipe read `pkg-config --exists libuv && $(MAKE) uv ... || echo "libuv not
found"`. When the libuv build failed, the `||` branch fired, `echo` returned 0,
and the recipe line succeeded. Both CI build jobs went green while printing a
message claiming libuv was absent on a runner where it was installed. `release`
carried the same shape for liburing. Replaced with `if`/`then`/`else`, where a
failing sub-make propagates. Verified by injecting a deliberate warning into a
libuv source: `make strict` now exits 2 where it previously exited 0.

The defect worth naming is not the shell operator. A check that discards its own
failure is worse than no check, because the green tick is now evidence of
nothing and nobody looks again.

**[bug] Four libuv sources never compiled under `-Werror`.**
Hidden by the entry above. `uv_server.c` and `uv_isprime_server.c` both left the
`handle` parameter of `on_alloc_buffer` unused and compared `buf->len` against
`nread` inside an `assert`, which is `size_t` against `ssize_t`. The timer demos
left callback parameters and `argc`/`argv` unused. None of them ever built
locally either, because the default target excludes `uv_%.c` and a machine
without `libuv1-dev` skips them silently.

**[bug] The metrics library was linked into every server and called by none.**
`metrics.c` and `metrics_server.c` compiled, linked, and passed their unit tests
without a single server calling `metrics_init` or `metrics_server_start`. The CI
smoke test scraped an endpoint that could not exist, waited 20 times, and failed
on an empty file with no explanation. `threadpool_server.c` now opens the
endpoint and instruments accept, refusal, close, queue depth and worker
occupancy, and `protocol.c` records service time per request.

**[bug] `make test-tsan` aborted before `main` inside a container.**
ThreadSanitizer maps its shadow memory at fixed addresses and needs ASLR entropy
at 28 bits or lower. Ubuntu raised `vm.mmap_rnd_bits` to 32. TSan normally
re-execs itself with randomisation off, so this stays invisible on a plain VM,
including the GitHub runner. A dev container blocks `personality(2)` in its
seccomp profile, the re-exec fails, and the binary aborts with an incompatible
memory layout. The first attempt ran the binary under `setarch -R`, which was wrong twice over:
the Codespace seccomp profile denies `personality(2)` to setarch as well, and
forcing it broke the case where kernel entropy was already low enough. `test-tsan`
now probes with `setarch -R true`, falls back to running direct when
`vm.mmap_rnd_bits` is 28 or lower, and fails with the remedy when neither holds.
It never skips. The container also gained `seccomp=unconfined` so the first path
works. Verified across all three branches by shadowing setarch with a failing
stub at both entropy settings.

**[style] `bench/run_bench.sh` failed shellcheck on SC2001.**
`echo "$out" | sed 's/^/    /'` for indentation, where a read loop does the same
without a subprocess. Harmless on its own, but CI had just brought the script
into scope and the job fails on a style finding like any other.

---

## Outstanding

`select_server.c` and `epoll_server.c` were left on their own state machines
while the benchmark was built, so the five models stayed comparable. They are the
next piece of work.

**[bug] Bounds checking inside an `assert`, both files.**
`assert(peerstate->sendbuf_end < SENDBUF_SIZE)` guards the write on the next
line. Under `-DNDEBUG` the guard disappears and the write past a 1024-byte buffer
happens silently, into the two `int` fields that follow it in the struct. Bounds
enforcement has to be a real `if` that returns an error. Asserts document
invariants, they do not enforce them.

**[bug] `EPOLLERR` on any peer socket kills the whole server.**
`perror_die("epoll_wait returned EPOLLERR")` fires for every fd in the ready set
carrying the error flag. A peer sending RST is routine, not exceptional.
`perror_die` is also misleading here, because `EPOLLERR` does not set `errno`, so
it prints whatever an earlier call left behind.

**[bug] Use of a closed fd in the same loop iteration, `select_server.c`.**
The read branch can `close(fd)` when the handler returns NORW. The write branch
then runs on the same fd in the same iteration, and if that fd was set in
`writefds` it calls `on_peer_ready_send` on a closed descriptor. `send` returns
EBADF and `perror_die` ends the process. `epoll_server.c` avoids this by using
`else if` on EPOLLOUT rather than a second independent check.

**[style] `#define MAXFDS 16 * 1024` is unparenthesised in `epoll_server.c`.**
Works where it is currently used, but `x / MAXFDS` would expand to
`x / 16 * 1024`, which is `(x / 16) * 1024`.

**[style] The EPOLLIN and EPOLLOUT branches are ~25 duplicated lines.**
Only the handler call differs. Pull the tail into a helper taking the fd and the
returned status.

**[bug] `uint8_t` used without `<stdint.h>` in both listeners.**
Compiles only because `utils.h` happens to pull it in transitively.

**[style] `const char** argv` in the listeners, `char** argv` in the servers.**

---

## Harness defects found while benchmarking

Three bugs in `bench/loadgen.c`, all mine, all found by looking at numbers that
did not make sense. Two of them changed what the results appeared to say.

**[bug] Throughput included connection setup.**
The wall clock started before the connections were made, so an accept-queue
overflow added its SYN retransmit delay to the throughput figure. This produced
an apparent throughput collapse at 128 connections in `threaded`, `select` and
`epoll` alike, all three landing within 6% of each other. No property of a
serving model does that. Connections now synchronise on a barrier and the request
clock starts after it, with setup reported separately as `setup_ms`,
`connect_max_us` and `ack_max_us`.

**[bug] The barrier sat after the server's ack, which deadlocked.**
A thread pool sends `*` only once a worker picks the connection up, and a worker
is not free until an earlier client finishes. Waiting for every ack before
releasing any client deadlocks against the server's own queueing. The thread pool
appeared to establish 4 of 128 connections. The barrier now sits after the TCP
handshake and before the ack, and time to first byte is measured separately.

**[bug] The non-blocking connect used `select`.**
Past 1024 file descriptors, `FD_SET` writes off the end of an `fd_set` and the
process aborts. The tool built to expose `select`'s limit had inherited it. Now
uses `poll`, which has no such ceiling.

**[bug] No timeouts anywhere.**
A blocking `connect` against a full accept queue retries for over a minute, and
`recv` on a silent server blocks forever. One saturated data point stalled the
whole sweep. Now a 5 second bounded connect and 10 second `SO_RCVTIMEO` and
`SO_SNDTIMEO`, with a `timeout` backstop per data point in `run_bench.sh`.

**[style] Each run overwrote the previous summary.**
Run one's CSV was lost to run two. Results now go to `results/<timestamp>/` with
`results/latest` as a symlink.

**[style] The charts flattered whichever server refused load fastest.**
A server accepting 134 of 1024 connections and serving those 134 quickly posts
the best throughput and the flattest latency line on the page. Points with failed
requests are now ringed, and `established.png` plots connections served against
connections offered with the ideal diagonal.

---

## Open measurement questions

Not defects. Results the second sweep produced that the first one contradicts, or
that one run is too thin to settle. They sit here rather than in the README as
findings, because a claim nobody has explained is not a finding yet.

**`select` and `epoll` stopped diverging at 256 connections.**
August: `epoll` served all 256 in 702ms with no errors, `select` took 10.2
seconds and failed 5,600 requests. September: both failed, at 4,391 and 4,907
requests per second, with no meaningful gap. Neither server changed. They keep
their own state machines, so the `protocol.c` instrumentation never reaches them.
What did change is the kernel, 6.8.0-1052-azure to 6.8.0-1064-azure, the compiler,
and the fd limit, 4096 to 8192. Checking the `established` column for both servers
at 256 is the first step: if both opened all 256 and only wall clock separates
them, the O(n) cost is still there and the errors come from somewhere else.

**`uring_server` was the only server to serve 256 connections cleanly.**
99,559 requests per second with no failed requests, against `epoll` at 4,907 with
failures, in the same sweep minutes apart. `connect_max_us` at 1.046 seconds says
its backlog overflowed like everyone else's, so the connections arrived late
rather than never. One run on a shared container, and it lands on the same 256
column as the question above, so treat the two as one cause until shown otherwise.

**The thread pool advantage should narrow as cores increase.**
The mechanism is scheduler contention between 128 threads and 2 runnable slots,
and nothing else. So 4 cores should roughly halve the 43x gap while leaving the
134-connection ceiling untouched, since that comes from backlog plus queue plus
workers and not from the hardware. Both sweeps so far are 2-core, so the
prediction is untested.

---

## Design limits, kept deliberately

These are not defects. They are what the benchmark exists to demonstrate.

**Sequential: one slow client blocks every other client.**
A peer that connects and never sends holds the server forever.

**Thread per connection: no ceiling on thread count.**
One thread per peer at roughly 8MB of virtual address space each. It held 100%
of offered connections through 1024, at a p50 of 3,850 microseconds and 111,200
failed requests.

**Thread pool: concurrency is capped at backlog + queue depth + workers.**
Measured at exactly 134 established when offered 256, 512 and 1024, against
`N_BACKLOG` 64, queue depth 64 and 4 workers. A worker owns a connection for its
whole lifetime, so the queue decouples accept from serve but does not raise the
concurrency ceiling.

**Thread pool: the acceptor blocks when the queue is full.**
Deliberate backpressure. The listen backlog absorbs the overflow, and once that
fills the kernel refuses connections, so overload is visible rather than hidden.
The queue counts how often the acceptor blocked.

**`select`: `FD_SETSIZE` caps the server at roughly 1000 connections,**
and the O(n) rebuild-and-scan per call is what breaks it at 256 on two cores.

**`epoll`: level-triggered, not edge-triggered.**
No `EPOLLET` anywhere, so the kernel keeps reporting readiness until the buffer
is drained, which is why the handlers can return after a single `recv`.

**`epoll`: `global_state` is 16384 entries of ~1KB, allocated up front.**
About 16MB whether one peer connects or none.

**Both listeners handle exactly one connection, then exit.**
They demonstrate blocking against non-blocking behaviour. The 200ms busy-poll in
the non-blocking one is the point, since it motivates `select`.

---

## Repo hygiene

- [x] `.gitignore`, `Makefile`, `.clang-format`
- [x] `utils.c` and `utils.h` tracked in git
- [x] GitHub Actions building with `-Werror`, sanitizers, and a format check
- [x] `.devcontainer/devcontainer.json` so the toolchain survives a rebuild, with the capabilities the sanitizers and the packet capture need
- [x] Python thread pool servers moved out of `src/` so the language bar reads C
- [x] README describing what the code does rather than how it was learned
- [x] Benchmark results committed with the environment they came from
- [x] Prometheus endpoint, alert rules and a provisioned Grafana dashboard under `deploy/`
- [x] Server-side counters reconciled against the load generator and against kernel `ListenOverflows`, agreeing to the connection
- [x] CI smoke test driving real traffic and reconciling server counters against the load generator

---

## Next

- Convert `select_server.c` and `epoll_server.c` onto a shared non-blocking parser, fixing the assert-bounds and `EPOLLERR` defects with it
- Raise `N_BACKLOG` from 64 and re-measure. The thread pool ceiling should move by exactly the amount added, which is a prediction from the code testable by measurement
- Repeat the sweeps on dedicated hardware to separate model behaviour from container noise
- Open the metrics endpoint on the remaining servers. The histogram already fills in `sequential_server` and `threaded_server` through `protocol.c`, but nothing serves it, so the dashboard reports one model where it should compare five
- Settle the two 256-connection questions above, with `tools/capture_stall.sh` and a packet trace rather than by re-running until one answer wins
- Add a kernel-side alert on `rate(node_netstat_TcpExt_ListenOverflows[1m])`. `ConnectionsBeingRefused` watches `server_connections_refused_total`, which the overload run proved stays at zero during backlog overflow, so the rule as written misses the failure mode this repo is about
