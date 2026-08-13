# Notes

Running list of issues found while working through the tutorial, and what I would
change. Nothing here is fixed yet. The tutorial code is kept faithful until all
four server models are in place, so the comparison between them is like for like.

Legend: **[bug]** breaks under real use, **[perf]** costs measurable throughput or
latency, **[style]** correctness-adjacent or maintainability, **[design]** inherent
to the model, keep and demonstrate.

---

## Cross-cutting

**[style] serve_connection is duplicated verbatim across servers.**
Byte for byte identical in `sequential.c` and `threaded.c`, including the same
bugs in both copies. Every fix has to be applied twice. Move the state machine
into `protocol.c` and have each server call it. The event-driven version needs
the same logic in a non-blocking shape, so the split has to happen anyway.

**[bug] SIGPIPE terminates the process.**
Writing to a socket whose peer has disconnected raises SIGPIPE, and the default
action kills the process. The `send` error branches never run because the process
is already gone. Fix with `signal(SIGPIPE, SIG_IGN)` at startup, or pass
`MSG_NOSIGNAL` on every send. Decide which and record why.

**[bug] EINTR treated as fatal.**
`recv` and `accept` return -1 with `errno == EINTR` when a signal arrives during
the call. Both servers call `perror_die` on any -1. Retry on EINTR instead.

**[bug] Inconsistent failure policy.**
A `recv` failure calls `perror_die` and takes the whole server down. A `send`
failure closes only that connection and returns. One client should never be able
to kill the process. Pick per-connection teardown as the policy and apply it
everywhere.

**[style] `int` used for `recv` return value.**
`recv` returns `ssize_t`. Harmless at a 1024-byte buffer, wrong in general, and a
narrowing conversion a reviewer will flag.

**[perf] One `send` syscall per byte.**
`send(sockfd, &buf[i], 1, 0)` inside the inner loop means a syscall and a context
switch for every character. Buffer the transformed bytes and send once per `recv`.
This is the single biggest measurable win in the project. Benchmark before and
after and put both numbers in the README.

**[bug] Partial sends unhandled.**
The `< 1` check works only because exactly one byte is sent. Once sends are
batched, `send` can return fewer bytes than requested. Loop until the buffer is
drained.

---

## sequential.c

**[design] One slow client blocks every other client.**
A peer that connects and never sends holds the server forever. This is the point
of the model. Keep it, and demonstrate it in the benchmark rather than fixing it.

---

## threaded.c

**[bug] File does not compile.**
- `typedef struct { int sockfd } thread_config__t;` — missing semicolon after
  `int sockfd`, and the type name has a double underscore while every use site
  says `thread_config_t`.
- `case IN_MSG;` — semicolon where the label needs a colon.

**[bug] `pthread_create` return value ignored.**
On failure the `config` allocation leaks and `newsockfd` is never closed. Check
the return code, free, close, continue.

**[design] No limit on thread count.**
One thread per connection at roughly 8MB of stack each. A few thousand
connections exhausts memory. This is the defining limitation of the model. Find
the breaking point with the load generator and record it.

**[note] Thread safety without locks.**
`state` and `buf` are local to `serve_connection`, so each thread has its own copy
on its own stack. No shared mutable state, therefore no mutex needed. Worth being
able to explain out loud.

---

## Repo hygiene

- [ ] `.gitignore` for objects, binaries, and build output
- [ ] `Makefile` so the repo builds from clone with one command
- [ ] GitHub Actions workflow building every server on push
- [ ] Repo description rewritten to say what the code does, not how it was learned
- [ ] Pick one language and say so — the tutorial is C

---

## Planned extension

The tutorial stops once the models work. The extension is measuring them.

- Load generator opening N concurrent connections and recording per-request
  latency
- Throughput and p50, p90, p99 latency across increasing connection counts
- Identify the crossover point where each model stops scaling, and why
- Written account of where each model breaks down and what the numbers showed

Optional, if time allows: an `io_uring` implementation benchmarked against the
`epoll` one.
