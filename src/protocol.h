// The framing protocol shared by every blocking server.
//
// On connect the server sends '*'. Thereafter '^' opens a frame and '$' closes
// it; bytes inside a frame are incremented by one and echoed back. Bytes
// outside a frame are discarded.
//
// This lives in one place so a fix lands once rather than once per server.

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <sys/types.h>

// Outcome of serving one connection. Callers log it; none of these are fatal to
// the process, which is the point: a misbehaving peer must not take down the
// other connections.
typedef enum {
  SERVE_OK,          // peer closed cleanly
  SERVE_PEER_RESET,  // peer vanished mid-conversation (ECONNRESET, EPIPE)
  SERVE_IO_ERROR     // anything else; errno is set
} serve_result_t;

// Human-readable name for a serve_result_t, for logging.
const char* serve_result_str(serve_result_t r);

// Runs the protocol on an already-accepted socket until the peer disconnects or
// an error occurs. Does NOT close sockfd; the caller owns it.
serve_result_t serve_connection(int sockfd);

// send() that keeps going until every byte is written or the write fails.
// Returns 0 on success, -1 with errno set otherwise. Retries on EINTR.
int send_all(int sockfd, const void* buf, size_t len);

// Makes SIGPIPE non-fatal for the process. Call once at startup in every
// server, or a peer that disconnects mid-write kills the process before the
// send error path is ever reached.
void ignore_sigpipe(void);

#endif /* PROTOCOL_H */
