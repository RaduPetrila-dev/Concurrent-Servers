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

**[bug] The tutorial builds with `-DNDEBUG`, so every assert is dead code.**
Two separate defects in `select_server.c` are invisible under the tutorial's own
Makefile for this reason: an undeclared identifier and a missing bounds check.
Build without `NDEBUG` by default, keep it for the `release` target only, and
treat any assert that guards a memory write as a bug rather than a check.

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

## select_server.c

**[bug] `MAXFDs` does not exist.**
The macro is `#define MAXFDS 1000`, but three asserts spell it `MAXFDs` with a
lowercase s. Confirmed: `gcc -Wall` rejects it with *'MAXFDs' undeclared*, and
`gcc -DNDEBUG` compiles it clean, because `assert` expands to nothing and the
identifier is never evaluated. The tutorial Makefile passes `-DNDEBUG`, which is
why this was never caught. Fix the spelling in all three places.

**[bug] Bounds checking lives inside an `assert`.**
`assert(peerstate->sendbuf_end < SENDBUF_SIZE)` guards the write on the next
line. Under `-DNDEBUG` that guard disappears and the write past the end of a
1024-byte buffer happens silently, corrupting `sendbuf_end` and `sendptr` which
sit immediately after it in the struct. Bounds enforcement has to be a real `if`
that returns an error. Asserts document invariants, they do not enforce them.

**[bug] Use of a closed fd in the same loop iteration.**
Inside the `for (fd ...)` loop, the read branch can `close(fd)` when the handler
returns NORW. The write branch then runs on the same fd in the same iteration,
and if that fd was set in `writefds` it calls `on_peer_ready_send` on a closed
descriptor. `send` returns EBADF and `perror_die` takes the process down. Skip
the write check with `continue` after closing, or track closure in a flag.

**[bug] `perror_die` on `recv` and `send` failure.**
Same fatal policy as the other servers. One misbehaving peer terminates every
connection on the process.

**[style] `assert(0 && "can't reach here")` for the INITIAL_ACK case.**
Under `-DNDEBUG` this becomes a silent fall-through rather than a crash. If the
state is genuinely unreachable, close the connection and log it instead.

**[style] `int` used for `recv` and `send` return values.**
Both return `ssize_t`.

**[design] `FD_SETSIZE` caps the server at roughly 1000 connections.**
Documented in the source and inherent to `select`. Keep it, and make the
benchmark show the wall.

**[design] `global_state[MAXFDS]` is a fixed ~1MB array indexed by fd.**
Simple and fast, and it means memory is allocated for peers that never connect.
Worth contrasting with a per-connection allocation in the write-up.

---

## blocking_listener.c and nonblocking_listener.c

**[bug] `uint8_t` used without including `<stdint.h>`.**
Both files declare `uint8_t buf[1024]` and neither includes `<stdint.h>`. This
compiles only because `utils.h` happens to pull it in. Include what you use.

**[style] `const char** argv` here, `char** argv` in the servers.**
Pick one and apply it everywhere. The standard form is `char** argv`.

**[style] `atoi` with no validation.**
A non-numeric argument silently becomes port 0. `strtol` with error checking is
two extra lines and is worth it in a file whose whole purpose is teaching.

**[design] Both handle exactly one connection, then exit.**
Correct. These are demonstrations of blocking versus non-blocking behaviour, not
servers. Leave them alone and say so in the README so a reader does not mistake
them for models under test.

**[design] The non-blocking listener busy-polls with a 200ms sleep.**
This is the point: it shows the cost of polling without an event notification
mechanism, and motivates `select`. Keep it and reference it when explaining why
`select_server.c` exists.

---

## epoll_server.c

**[bug] `EPOLLERR` on any peer socket kills the whole server.**
`perror_die("epoll_wait returned EPOLLERR")` fires for every fd in the ready set
that has the error flag. A peer sending RST is routine, not exceptional, so a
single client closing hard takes down every other connection. Close that fd,
remove it from the epoll set, and carry on. `perror_die` is also misleading
here, because `EPOLLERR` does not set `errno`, so it prints whatever was left
over from an earlier call.

**[bug] Bounds check inside an `assert` again.**
`assert(peerstate->sendbuf_end < SENDBUF_SIZE)` guards the write on the next
line, and vanishes under `-DNDEBUG`. Same defect as `select_server.c`, same fix:
make it a real `if` that returns an error.

**[style] `#define MAXFDS 16 * 1024` is unparenthesised.**
It works everywhere it is currently used because the surrounding operators are
lower precedence, but `x / MAXFDS` would expand to `x / 16 * 1024`, which is
`(x / 16) * 1024`. Wrap the whole thing: `#define MAXFDS (16 * 1024)`.

**[style] The EPOLLIN and EPOLLOUT branches are ~25 duplicated lines.**
Both build a `struct epoll_event`, set flags from the returned `fd_status_t`,
then either `EPOLL_CTL_DEL` and close, or `EPOLL_CTL_MOD`. Only the handler call
differs. Pull the tail into a helper taking the fd and the status.

**[style] `int` for `recv` and `send` return values.**
`ssize_t` in both places, as elsewhere.

**[design] Level-triggered, not edge-triggered.**
No `EPOLLET` anywhere, so the kernel keeps reporting readiness until the buffer
is drained. That is the right default and it is why the handlers can return
after a single `recv`. Worth stating in the README, and worth benchmarking an
`EPOLLET` variant later.

**[design] `global_state` is 16384 entries of ~1KB, allocated up front.**
About 16MB of static memory whether one peer connects or none. Same trade as
`select_server.c` at 16x the scale. Contrast with per-connection allocation in
the write-up.

**[note] The listener and peer branches are correctly separated.**
`if (events[i].data.fd == listener_sockfd) { ... } else { ... }`, with the accept
path handling `EAGAIN` properly and rejecting fds at or above `MAXFDS`. No
equivalent of the `select_server.c` use-after-close bug, because the peer branch
uses `else if` on EPOLLOUT rather than a second independent check.

---

## Repo hygiene

- [x] `.gitignore` for objects, binaries, and build output
- [x] `Makefile` so the repo builds from clone with one command
- [x] `.clang-format` matching the existing style
- [x] `utils.c` and `utils.h` tracked in git
- [x] GitHub Actions workflow building every server on push
- [x] `.devcontainer/devcontainer.json` so clang-format and libuv survive a rebuild
- [ ] README: currently one heading and nothing else
- [ ] Repo description rewritten to say what the code does, not how it was learned
- [x] Rename `Notes.md` to `NOTES.md` to match README and LICENSE
- [x] Move the Python thread pool files out of `src/` so the language bar reads C
- [ ] No C thread pool server — `threadpool_server.c` does not exist, only the two Python ones
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
