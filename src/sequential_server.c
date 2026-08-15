// Sequential socket server - accepting one client at a time.
//
// The simplest model, and the baseline every other server is measured against.
// One client is served to completion before the next is accepted, so a slow
// peer blocks everybody behind it.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.h"
#include "utils.h"

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ignore_sigpipe();

  int portnum = 9090;
  if (argc >= 2) {
    portnum = atoi(argv[1]);
  }
  printf("Serving on port %d\n", portnum);

  int sockfd = listen_inet_socket(portnum);

  while (1) {
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    int newsockfd =
        accept(sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);
    if (newsockfd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      perror_die("accept");
    }

    report_peer_connected(&peer_addr, peer_addr_len);
    serve_result_t r = serve_connection(newsockfd);
    close(newsockfd);
    printf("peer done (%s)\n", serve_result_str(r));
  }

  return 0;
}
