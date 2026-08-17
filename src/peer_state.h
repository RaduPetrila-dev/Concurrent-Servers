// Per-peer protocol state for the event-driven servers.
//
// The blocking servers can sit in a loop inside serve_connection() because they
// own a thread. select, epoll and io_uring cannot: a handler has to return
// before a message is necessarily complete, so the parser state and any pending
// output live per file descriptor and survive between callbacks.
//
// The framing is the same as protocol.h: '*' on connect, '^' opens a frame, '$'
// closes it, bytes inside a frame are incremented by one and echoed.
//
// The interface is split in two on purpose.
//
//   peer_consume / peer_pending / peer_sent are pure: bytes in, bytes out, no
//   syscalls. io_uring needs these, because by the time a completion arrives
//   the kernel has already moved the data and there is nothing left to read.
//
//   peer_on_ready_recv / peer_on_ready_send wrap those in a recv and a send,
//   which is what a readiness-based loop like select or epoll wants.
//
// Both sit on one state machine, so a fix lands once for all three servers.

#ifndef PEER_STATE_H
#define PEER_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// What the event loop should do with this descriptor next. Both false means the
// connection is finished; ask peer_close_reason() why before closing it.
typedef struct {
  bool want_read;
  bool want_write;
} fd_status_t;

extern const fd_status_t fd_status_R;
extern const fd_status_t fd_status_W;
extern const fd_status_t fd_status_RW;
extern const fd_status_t fd_status_NORW;

// Allocates the per-fd table. max_fds must exceed any descriptor the loop will
// hand back. Call once at startup; dies on allocation failure.
void peer_state_init(size_t max_fds);

// True when sockfd is inside the table. Callers must reject anything else
// before accepting it, or the table is indexed out of bounds.
bool peer_fd_in_range(int sockfd);

// Why the last call returned fd_status_NORW, for logging before the close.
const char* peer_close_reason(int sockfd);

// Resets state for a newly accepted peer and stages the '*' acknowledgement. A
// descriptor is unique only while its connection is open, so this clears every
// trace of whichever peer held the number before.
fd_status_t peer_on_connected(int sockfd);

// Records a failure the caller detected itself, so the close reason is
// consistent whichever layer noticed.
fd_status_t peer_fail(int sockfd, const char* reason);

// --- pure core, no syscalls ---

// Largest number of bytes that may be handed to peer_consume in one call. Each
// input byte produces at most one output byte, so respecting this makes the
// output buffer provably safe and removes the need for a bounds check inside
// the parser. The tutorial guarded that write with an assert, which -DNDEBUG
// deletes from the build that ships.
size_t peer_recv_room(int sockfd);

// Feeds received bytes through the state machine. len must not exceed
// peer_recv_room(sockfd).
fd_status_t peer_consume(int sockfd, const uint8_t* data, size_t len);

// Points *out at whatever is staged for sending and returns its length, or 0.
size_t peer_pending(int sockfd, const uint8_t** out);

// Records that n bytes of the pending output were written.
fd_status_t peer_sent(int sockfd, size_t n);

// --- readiness wrappers, for select and epoll ---

fd_status_t peer_on_ready_recv(int sockfd);
fd_status_t peer_on_ready_send(int sockfd);

#endif /* PEER_STATE_H */
