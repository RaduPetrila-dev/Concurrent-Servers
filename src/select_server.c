// Asynchronous socket server - accepting multiple clients concurrently,
// multiplexing the connections with select.
//
// One thread, one loop. select() reports which descriptors are ready, and the
// per-peer parser in peer_state.c keeps the framing state between callbacks.
//
// The model's ceiling is FD_SETSIZE, 1024 on Linux and awkward to change, plus
// the O(n) cost of rebuilding and walking the sets on every call.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "peer_state.h"
#include "protocol.h"
#include "utils.h"

#define MAXFDS 1000

// Applies a handler's verdict to the master sets, closing the peer when it has
// nothing left to do either way.
static void apply_status(int fd, fd_status_t status, fd_set* readfds_master,
                         fd_set* writefds_master) {
  if (status.want_read) {
    FD_SET(fd, readfds_master);
  } else {
    FD_CLR(fd, readfds_master);
  }

  if (status.want_write) {
    FD_SET(fd, writefds_master);
  } else {
    FD_CLR(fd, writefds_master);
  }

  if (!status.want_read && !status.want_write) {
    printf("socket %d closing (%s)\n", fd, peer_close_reason(fd));
    close(fd);
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ignore_sigpipe();

  int portnum = 9090;
  if (argc >= 2) {
    portnum = atoi(argv[1]);
  }
  printf("Serving on port %d\n", portnum);

  peer_state_init(MAXFDS);

  int listener_sockfd = listen_inet_socket(portnum);

  // select() can report a socket readable when it is not, so blocking I/O on
  // these descriptors is unsafe.
  make_socket_non_blocking(listener_sockfd);

  if (listener_sockfd >= FD_SETSIZE) {
    die("listener socket fd (%d) >= FD_SETSIZE (%d)", listener_sockfd,
        FD_SETSIZE);
  }

  fd_set readfds_master;
  fd_set writefds_master;
  FD_ZERO(&readfds_master);
  FD_ZERO(&writefds_master);

  // The listening socket is always watched for read, to see new peers arrive.
  FD_SET(listener_sockfd, &readfds_master);

  // Tracking the highest descriptor seen saves select() walking all the way to
  // FD_SETSIZE on every call.
  int fdset_max = listener_sockfd;

  while (1) {
    // select() rewrites the sets it is given, so it gets copies.
    fd_set readfds = readfds_master;
    fd_set writefds = writefds_master;

    int nready = select(fdset_max + 1, &readfds, &writefds, NULL, NULL);
    if (nready < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror_die("select");
    }

    // nready counts ready events, so a socket that is both readable and
    // writable contributes two.
    for (int fd = 0; fd <= fdset_max && nready > 0; fd++) {
      bool closed = false;

      if (FD_ISSET(fd, &readfds)) {
        nready--;

        if (fd == listener_sockfd) {
          struct sockaddr_in peer_addr;
          socklen_t peer_addr_len = sizeof(peer_addr);
          int newsockfd = accept(listener_sockfd, (struct sockaddr*)&peer_addr,
                                 &peer_addr_len);
          if (newsockfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
                errno == ECONNABORTED) {
              // Ordinary on a non-blocking listener. Nothing to do.
            } else {
              perror_die("accept");
            }
          } else if (newsockfd >= FD_SETSIZE || !peer_fd_in_range(newsockfd)) {
            // Refuse rather than index the peer table out of bounds or write
            // past the end of an fd_set.
            printf("rejecting socket %d, beyond the %d descriptor limit\n",
                   newsockfd, MAXFDS < FD_SETSIZE ? MAXFDS : FD_SETSIZE);
            close(newsockfd);
          } else {
            make_socket_non_blocking(newsockfd);
            if (newsockfd > fdset_max) {
              fdset_max = newsockfd;
            }
            report_peer_connected(&peer_addr, peer_addr_len);
            apply_status(newsockfd, peer_on_connected(newsockfd),
                         &readfds_master, &writefds_master);
          }
        } else {
          fd_status_t status = peer_on_ready_recv(fd);
          closed = !status.want_read && !status.want_write;
          apply_status(fd, status, &readfds_master, &writefds_master);
        }
      }

      // The read branch above may have closed this descriptor. Checking it for
      // writability afterwards would call send() on a closed fd, get EBADF, and
      // in the original code end the process.
      if (!closed && FD_ISSET(fd, &writefds)) {
        nready--;
        apply_status(fd, peer_on_ready_send(fd), &readfds_master,
                     &writefds_master);
      }
    }
  }

  return 0;
}
