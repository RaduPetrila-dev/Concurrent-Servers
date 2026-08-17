// Asynchronous socket server - multiplexing connections with io_uring.
//
// epoll asks the kernel which descriptors are ready; the process then makes the
// syscall itself. io_uring inverts that. The process submits the operation it
// wants performed and the kernel returns the result once it is done, through
// two ring buffers shared between them. A batch of I/O costs one
// io_uring_enter rather than one syscall per operation.
//
// That difference is why peer_state.c is split. By the time a completion
// arrives the kernel has already moved the bytes, so there is nothing left to
// read: this server uses the pure peer_consume, peer_pending and peer_sent,
// while select and epoll use the recv and send wrappers around them. One state
// machine, two ways of driving it.
//
// Usage: uring_server [port] [queue_depth]   Defaults: 9090 256

#include <errno.h>
#include <liburing.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "peer_state.h"
#include "protocol.h"
#include "utils.h"

#define MAXFDS (16 * 1024)
#define IOBUF_SIZE 1024
#define DEFAULT_QUEUE_DEPTH 256

// Each submission carries 64 bits of user data that comes back untouched on
// completion, and that is the only way to know which operation finished. The
// descriptor goes in the low half and the operation in the high half.
typedef enum { OP_ACCEPT = 1, OP_RECV = 2, OP_SEND = 3 } uring_op_t;

static uint64_t pack(uring_op_t op, int fd) {
  return ((uint64_t)op << 32) | (uint32_t)fd;
}

static uring_op_t unpack_op(uint64_t data) {
  return (uring_op_t)(data >> 32);
}

static int unpack_fd(uint64_t data) {
  return (int)(uint32_t)data;
}

// The kernel writes into these asynchronously, so they must outlive the
// submission. One per descriptor is the simplest arrangement that guarantees
// that without reference counting.
static uint8_t (*iobufs)[IOBUF_SIZE];

static struct sockaddr_in accept_addr;
static socklen_t accept_addr_len = sizeof(accept_addr);

static struct io_uring_sqe* get_sqe(struct io_uring* ring) {
  struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
  if (sqe == NULL) {
    // The submission queue is full. Flushing frees slots; if it is still full
    // after that, the configured depth is too small for the offered load.
    io_uring_submit(ring);
    sqe = io_uring_get_sqe(ring);
    if (sqe == NULL) {
      die("submission queue exhausted, raise the queue depth");
    }
  }
  return sqe;
}

static void queue_accept(struct io_uring* ring, int listener) {
  struct io_uring_sqe* sqe = get_sqe(ring);
  accept_addr_len = sizeof(accept_addr);
  io_uring_prep_accept(sqe, listener, (struct sockaddr*)&accept_addr,
                       &accept_addr_len, 0);
  io_uring_sqe_set_data64(sqe, pack(OP_ACCEPT, listener));
}

static void queue_recv(struct io_uring* ring, int fd) {
  size_t room = peer_recv_room(fd);
  if (room > IOBUF_SIZE) {
    room = IOBUF_SIZE;
  }
  struct io_uring_sqe* sqe = get_sqe(ring);
  io_uring_prep_recv(sqe, fd, iobufs[fd], room, 0);
  io_uring_sqe_set_data64(sqe, pack(OP_RECV, fd));
}

static void queue_send(struct io_uring* ring, int fd) {
  const uint8_t* out = NULL;
  size_t len = peer_pending(fd, &out);
  if (len == 0) {
    queue_recv(ring, fd);
    return;
  }
  // The parser owns its send buffer and may rewrite it once the send completes,
  // so the bytes are copied into the descriptor's own buffer first.
  if (len > IOBUF_SIZE) {
    len = IOBUF_SIZE;
  }
  memcpy(iobufs[fd], out, len);

  struct io_uring_sqe* sqe = get_sqe(ring);
  io_uring_prep_send(sqe, fd, iobufs[fd], len, 0);
  io_uring_sqe_set_data64(sqe, pack(OP_SEND, fd));
}

// Turns a parser verdict into the next submission, or closes the connection.
static void arm_next(struct io_uring* ring, int fd, fd_status_t status) {
  if (!status.want_read && !status.want_write) {
    printf("socket %d closing (%s)\n", fd, peer_close_reason(fd));
    close(fd);
    return;
  }
  if (status.want_write) {
    queue_send(ring, fd);
  } else {
    queue_recv(ring, fd);
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ignore_sigpipe();

  int portnum = 9090;
  unsigned queue_depth = DEFAULT_QUEUE_DEPTH;
  if (argc >= 2) {
    portnum = atoi(argv[1]);
  }
  if (argc >= 3) {
    queue_depth = (unsigned)atoi(argv[2]);
  }
  if (queue_depth < 8) {
    die("queue_depth must be at least 8");
  }

  printf("Serving on port %d with a queue depth of %u\n", portnum, queue_depth);

  peer_state_init(MAXFDS);
  iobufs = calloc(MAXFDS, IOBUF_SIZE);
  if (iobufs == NULL) {
    die("could not allocate the per-descriptor I/O buffers");
  }

  struct io_uring ring;
  int rc = io_uring_queue_init(queue_depth, &ring, 0);
  if (rc < 0) {
    die("io_uring_queue_init failed: %s", strerror(-rc));
  }

  int listener = listen_inet_socket(portnum);
  queue_accept(&ring, listener);

  while (1) {
    io_uring_submit(&ring);

    struct io_uring_cqe* cqe;
    rc = io_uring_wait_cqe(&ring, &cqe);
    if (rc < 0) {
      if (rc == -EINTR) {
        continue;
      }
      die("io_uring_wait_cqe failed: %s", strerror(-rc));
    }

    unsigned head;
    unsigned processed = 0;

    // Drain every completion the kernel has posted before submitting again.
    // Batching both directions is what makes the ring worth the complexity.
    io_uring_for_each_cqe(&ring, head, cqe) {
      processed++;
      uint64_t data = io_uring_cqe_get_data64(cqe);
      int fd = unpack_fd(data);
      int res = cqe->res;

      switch (unpack_op(data)) {
      case OP_ACCEPT:
        // Re-arm first, so the listener is never left unwatched.
        queue_accept(&ring, listener);

        if (res < 0) {
          if (res != -EINTR && res != -ECONNABORTED && res != -EAGAIN) {
            fprintf(stderr, "accept failed: %s\n", strerror(-res));
          }
          break;
        }
        if (!peer_fd_in_range(res)) {
          printf("rejecting socket %d, beyond the %d descriptor limit\n", res,
                 MAXFDS);
          close(res);
          break;
        }
        report_peer_connected(&accept_addr, accept_addr_len);
        arm_next(&ring, res, peer_on_connected(res));
        break;

      case OP_RECV:
        if (res < 0) {
          if (res == -EINTR || res == -EAGAIN) {
            queue_recv(&ring, fd);
            break;
          }
          arm_next(&ring, fd, peer_fail(fd, strerror(-res)));
          break;
        }
        if (res == 0) {
          arm_next(&ring, fd, peer_fail(fd, "peer closed"));
          break;
        }
        // The kernel has already copied the bytes in, so the parser is fed
        // directly rather than being asked to read them.
        arm_next(&ring, fd, peer_consume(fd, iobufs[fd], (size_t)res));
        break;

      case OP_SEND:
        if (res < 0) {
          if (res == -EINTR || res == -EAGAIN) {
            queue_send(&ring, fd);
            break;
          }
          arm_next(&ring, fd, peer_fail(fd, strerror(-res)));
          break;
        }
        // A short write is normal; peer_sent reports whether more is pending.
        arm_next(&ring, fd, peer_sent(fd, (size_t)res));
        break;
      }
    }

    io_uring_cq_advance(&ring, processed);
  }

  io_uring_queue_exit(&ring);
  return 0;
}
