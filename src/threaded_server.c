// Threaded socket server - accepting multiple clients concurrently, by
// creating a new detached thread for each client.
//
// Scales until thread stacks exhaust memory. Each thread owns its own parser
// state on its own stack, so no shared mutable state exists and no lock is
// needed anywhere in this file.

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.h"
#include "utils.h"

typedef struct {
  int sockfd;
} thread_config_t;

static void* server_thread(void* arg) {
  thread_config_t* config = (thread_config_t*)arg;
  int sockfd = config->sockfd;
  free(config);

  unsigned long id = (unsigned long)pthread_self();
  printf("Thread %lu created to handle connection with socket %d\n", id,
         sockfd);

  serve_result_t r = serve_connection(sockfd);
  close(sockfd);

  printf("Thread %lu done (%s)\n", id, serve_result_str(r));
  return NULL;
}

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

    thread_config_t* config = xmalloc(sizeof(*config));
    config->sockfd = newsockfd;

    pthread_t the_thread;
    int rc = pthread_create(&the_thread, NULL, server_thread, config);
    if (rc != 0) {
      // Out of threads. Drop this peer rather than the whole server.
      fprintf(stderr, "pthread_create failed (%d), dropping socket %d\n", rc,
              newsockfd);
      free(config);
      close(newsockfd);
      continue;
    }
    pthread_detach(the_thread);
  }

  return 0;
}
