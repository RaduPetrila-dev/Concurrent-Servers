// Asynchronous socket server - accepting multiple clients concurrently,
// multiplexing the connections with epoll.
//
// Same single loop as select_server.c, with the kernel keeping the interest set
// instead of the process rebuilding it on every call. That removes the O(n)
// scan and the FD_SETSIZE ceiling, which is the whole reason epoll exists.
//
// Level-triggered, not edge-triggered: no EPOLLET anywhere, so the kernel keeps
// reporting readiness until the buffer is drained. That is why a handler can
// return after a single recv without stalling the connection.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "peer_state.h"
#include "protocol.h"
#include "utils.h"

#define MAXFDS (16 * 1024)

// Applies a handler's verdict to the epoll interest set, removing and closing
// the peer when it has nothing left to do either way.
static void apply_status(int epollfd, int fd, fd_status_t status) {
  if (!status.want_read && !status.want_write) {
    printf("socket %d closing (%s)\n", fd, peer_close_reason(fd));
    // EPOLL_CTL_DEL before close is not strictly required, since closing the
    // descriptor removes it, but it keeps the intent explicit and survives the
    // fd being duplicated elsewhere.
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
    return;
  }

  struct epoll_event event = {.data = {.fd = fd}, .events = 0};
  if (status.want_read) {
    event.events |= EPOLLIN;
  }
  if (status.want_write) {
    event.events |= EPOLLOUT;
  }

  if (epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event) < 0) {
    perror_die("epoll_ctl EPOLL_CTL_MOD");
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
  make_socket_non_blocking(listener_sockfd);

  int epollfd = epoll_create1(0);
  if (epollfd < 0) {
    perror_die("epoll_create1");
  }

  struct epoll_event accept_event = {.data = {.fd = listener_sockfd},
                                     .events = EPOLLIN};
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listener_sockfd, &accept_event) < 0) {
    perror_die("epoll_ctl EPOLL_CTL_ADD");
  }

  struct epoll_event* events = calloc(MAXFDS, sizeof(struct epoll_event));
  if (events == NULL) {
    die("could not allocate the epoll event buffer");
  }

  while (1) {
    int nready = epoll_wait(epollfd, events, MAXFDS, -1);
    if (nready < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror_die("epoll_wait");
    }

    for (int i = 0; i < nready; i++) {
      int fd = events[i].data.fd;

      if (events[i].events & EPOLLERR) {
        // A peer sending RST raises this routinely, so it is not exceptional.
        // The original called perror_die here, which meant one client resetting
        // its connection ended every other connection on the process. EPOLLERR
        // also does not set errno, so perror printed a stale message.
        if (fd == listener_sockfd) {
          die("error on the listening socket");
        }
        printf("socket %d closing (epoll reported an error)\n", fd);
        epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
        close(fd);
        continue;
      }

      if (fd == listener_sockfd) {
        struct sockaddr_in peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        int newsockfd = accept(listener_sockfd, (struct sockaddr*)&peer_addr,
                               &peer_addr_len);
        if (newsockfd < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
              errno == ECONNABORTED) {
            // Ordinary on a non-blocking listener.
            continue;
          }
          perror_die("accept");
        }

        if (!peer_fd_in_range(newsockfd)) {
          printf("rejecting socket %d, beyond the %d descriptor limit\n",
                 newsockfd, MAXFDS);
          close(newsockfd);
          continue;
        }

        make_socket_non_blocking(newsockfd);
        report_peer_connected(&peer_addr, peer_addr_len);

        fd_status_t status = peer_on_connected(newsockfd);
        struct epoll_event event = {.data = {.fd = newsockfd}, .events = 0};
        if (status.want_read) {
          event.events |= EPOLLIN;
        }
        if (status.want_write) {
          event.events |= EPOLLOUT;
        }
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, newsockfd, &event) < 0) {
          perror_die("epoll_ctl EPOLL_CTL_ADD");
        }
        continue;
      }

      // A peer. EPOLLIN is handled first, and its verdict decides whether the
      // descriptor is still open, so EPOLLOUT is an else-if rather than a
      // second independent check.
      if (events[i].events & EPOLLIN) {
        apply_status(epollfd, fd, peer_on_ready_recv(fd));
      } else if (events[i].events & EPOLLOUT) {
        apply_status(epollfd, fd, peer_on_ready_send(fd));
      }
    }
  }

  return 0;
}
