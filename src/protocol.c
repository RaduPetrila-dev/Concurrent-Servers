#include "protocol.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>

#define RECVBUF_SIZE 1024

typedef enum { WAIT_FOR_MSG, IN_MSG } ProcessingState;

const char* serve_result_str(serve_result_t r) {
  switch (r) {
  case SERVE_OK:
    return "peer closed";
  case SERVE_PEER_RESET:
    return "peer reset";
  case SERVE_IO_ERROR:
    return "io error";
  }
  return "unknown";
}

void ignore_sigpipe(void) {
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGPIPE, &sa, NULL);
}

int send_all(int sockfd, const void* buf, size_t len) {
  const uint8_t* p = (const uint8_t*)buf;
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(sockfd, p + sent, len - sent, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    sent += (size_t)n;
  }
  return 0;
}

// Maps an errno from a failed recv/send onto a serve_result_t.
static serve_result_t classify_errno(void) {
  if (errno == ECONNRESET || errno == EPIPE || errno == ETIMEDOUT) {
    return SERVE_PEER_RESET;
  }
  return SERVE_IO_ERROR;
}

serve_result_t serve_connection(int sockfd) {
  if (send_all(sockfd, "*", 1) < 0) {
    return classify_errno();
  }

  ProcessingState state = WAIT_FOR_MSG;

  while (1) {
    uint8_t recvbuf[RECVBUF_SIZE];
    ssize_t len = recv(sockfd, recvbuf, sizeof recvbuf, 0);
    if (len < 0) {
      if (errno == EINTR) {
        continue;
      }
      return classify_errno();
    } else if (len == 0) {
      return SERVE_OK;
    }

    // Transform in place and send once per recv, rather than one send() syscall
    // per byte. sendbuf can never exceed the number of bytes just received, so
    // reusing recvbuf is safe and needs no bounds check.
    size_t nsend = 0;
    for (ssize_t i = 0; i < len; ++i) {
      uint8_t c = recvbuf[i];
      switch (state) {
      case WAIT_FOR_MSG:
        if (c == '^') {
          state = IN_MSG;
        }
        break;
      case IN_MSG:
        if (c == '$') {
          state = WAIT_FOR_MSG;
        } else {
          recvbuf[nsend++] = c + 1;
        }
        break;
      }
    }

    if (nsend > 0 && send_all(sockfd, recvbuf, nsend) < 0) {
      return classify_errno();
    }
  }
}
