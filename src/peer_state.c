#include "peer_state.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "utils.h"

#define SENDBUF_SIZE 1024

typedef enum { INITIAL_ACK, WAIT_FOR_MSG, IN_MSG } processing_state_t;

typedef struct {
  processing_state_t state;

  // Output staged for the peer. peer_consume fills it, peer_sent drains it.
  // sendptr is the next byte to write, sendbuf_end one past the last valid one.
  uint8_t sendbuf[SENDBUF_SIZE];
  size_t sendbuf_end;
  size_t sendptr;

  const char* close_reason;
} peer_state_t;

static peer_state_t* peers = NULL;
static size_t peer_capacity = 0;

const fd_status_t fd_status_R = {.want_read = true, .want_write = false};
const fd_status_t fd_status_W = {.want_read = false, .want_write = true};
const fd_status_t fd_status_RW = {.want_read = true, .want_write = true};
const fd_status_t fd_status_NORW = {.want_read = false, .want_write = false};

void peer_state_init(size_t max_fds) {
  peers = calloc(max_fds, sizeof(peer_state_t));
  if (peers == NULL) {
    die("could not allocate peer state for %zu descriptors", max_fds);
  }
  peer_capacity = max_fds;
}

bool peer_fd_in_range(int sockfd) {
  return sockfd >= 0 && (size_t)sockfd < peer_capacity;
}

const char* peer_close_reason(int sockfd) {
  if (!peer_fd_in_range(sockfd) || peers[sockfd].close_reason == NULL) {
    return "peer closed";
  }
  return peers[sockfd].close_reason;
}

// Ends the connection with a reason the caller can log. Returning a status
// rather than calling perror_die is the point of the whole module: one peer
// resetting must not take down every other connection on the process.
static fd_status_t close_with(peer_state_t* peer, const char* reason) {
  peer->close_reason = reason;
  return fd_status_NORW;
}

fd_status_t peer_on_connected(int sockfd) {
  peer_state_t* peer = &peers[sockfd];

  // A descriptor is reused once its previous owner closes, so every trace of
  // the old peer has to go.
  peer->state = INITIAL_ACK;
  peer->sendbuf[0] = '*';
  peer->sendptr = 0;
  peer->sendbuf_end = 1;
  peer->close_reason = NULL;

  return fd_status_W;
}

fd_status_t peer_fail(int sockfd, const char* reason) {
  return close_with(&peers[sockfd], reason);
}

// --- pure core, no syscalls ---

size_t peer_recv_room(int sockfd) {
  peer_state_t* peer = &peers[sockfd];
  if (peer->state == INITIAL_ACK || peer->sendptr < peer->sendbuf_end) {
    // Nothing is wanted from the peer until the acknowledgement has gone out
    // and everything already staged has been drained.
    return 0;
  }
  return SENDBUF_SIZE - peer->sendbuf_end;
}

fd_status_t peer_consume(int sockfd, const uint8_t* data, size_t len) {
  peer_state_t* peer = &peers[sockfd];

  if (len > peer_recv_room(sockfd)) {
    // Handing over more than the caller asked room for is a bug in the caller
    // rather than a peer problem, so it fails loudly instead of truncating.
    return close_with(peer, "internal error: consume beyond available room");
  }

  bool ready_to_send = false;
  for (size_t i = 0; i < len; ++i) {
    switch (peer->state) {
    case INITIAL_ACK:
      // Unreachable: peer_recv_room returns 0 in this state.
      return close_with(peer, "internal error: consume while INITIAL_ACK");
    case WAIT_FOR_MSG:
      if (data[i] == '^') {
        peer->state = IN_MSG;
      }
      break;
    case IN_MSG:
      if (data[i] == '$') {
        peer->state = WAIT_FOR_MSG;
      } else {
        // Provably in range: len is capped at the remaining room and each input
        // byte produces at most one output byte, so no bounds check is needed
        // here. The tutorial used an assert, which -DNDEBUG removes.
        peer->sendbuf[peer->sendbuf_end++] = data[i] + 1;
        ready_to_send = true;
      }
      break;
    }
  }

  // Only ask for more input once anything produced has been handed back.
  return (fd_status_t){.want_read = !ready_to_send,
                       .want_write = ready_to_send};
}

size_t peer_pending(int sockfd, const uint8_t** out) {
  peer_state_t* peer = &peers[sockfd];
  if (peer->sendptr >= peer->sendbuf_end) {
    *out = NULL;
    return 0;
  }
  *out = &peer->sendbuf[peer->sendptr];
  return peer->sendbuf_end - peer->sendptr;
}

fd_status_t peer_sent(int sockfd, size_t n) {
  peer_state_t* peer = &peers[sockfd];
  peer->sendptr += n;

  if (peer->sendptr < peer->sendbuf_end) {
    return fd_status_W;
  }

  peer->sendptr = 0;
  peer->sendbuf_end = 0;

  if (peer->state == INITIAL_ACK) {
    peer->state = WAIT_FOR_MSG;
  }

  return fd_status_R;
}

// --- readiness wrappers, for select and epoll ---

fd_status_t peer_on_ready_recv(int sockfd) {
  peer_state_t* peer = &peers[sockfd];

  size_t room = peer_recv_room(sockfd);
  if (room == 0) {
    return fd_status_W;
  }

  uint8_t buf[SENDBUF_SIZE];
  size_t want = room < sizeof buf ? room : sizeof buf;

  ssize_t nbytes = recv(sockfd, buf, want, 0);
  if (nbytes == 0) {
    return close_with(peer, "peer closed");
  }
  if (nbytes < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      // select() documents reporting a socket readable when it is not, so this
      // is expected rather than exceptional. Wait for the next notification.
      return fd_status_R;
    }
    if (errno == ECONNRESET) {
      return close_with(peer, "peer reset");
    }
    return close_with(peer, strerror(errno));
  }

  return peer_consume(sockfd, buf, (size_t)nbytes);
}

fd_status_t peer_on_ready_send(int sockfd) {
  peer_state_t* peer = &peers[sockfd];

  const uint8_t* out = NULL;
  size_t sendlen = peer_pending(sockfd, &out);
  if (sendlen == 0) {
    return fd_status_RW;
  }

  ssize_t nsent = send(sockfd, out, sendlen, 0);
  if (nsent < 0) {
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
      return fd_status_W;
    }
    if (errno == ECONNRESET || errno == EPIPE) {
      return close_with(peer, "peer reset");
    }
    return close_with(peer, strerror(errno));
  }

  return peer_sent(sockfd, (size_t)nsent);
}
