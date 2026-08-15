// Thread pool socket server.
//
// A fixed number of worker threads pull accepted connections off a bounded
// queue. This removes the thread-per-connection ceiling in threaded_server.c:
// memory and scheduler cost stop growing with the number of clients, and the
// queue provides backpressure when clients arrive faster than workers finish.
//
// Usage: threadpool_server [port] [num_workers] [queue_depth]
// Defaults:                9090   4             64

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.h"
#include "utils.h"

#define DEFAULT_PORT 9090
#define DEFAULT_WORKERS 4
#define DEFAULT_QUEUE_DEPTH 64

// A bounded circular queue of accepted file descriptors.
//
// Guarded by one mutex and two condition variables: not_empty wakes a worker
// when work arrives, not_full wakes the acceptor when a slot frees up. Two
// separate conditions rather than one avoids waking threads that cannot make
// progress.
typedef struct {
  int* fds;
  size_t capacity;
  size_t head;   // next slot to read
  size_t tail;   // next slot to write
  size_t count;  // entries currently queued
  pthread_mutex_t lock;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;

  // Counters for the backpressure story. Read under lock.
  unsigned long long total_enqueued;
  unsigned long long acceptor_blocked;  // times the acceptor had to wait
} fd_queue_t;

static void queue_init(fd_queue_t* q, size_t capacity) {
  q->fds = xmalloc(capacity * sizeof(int));
  q->capacity = capacity;
  q->head = q->tail = q->count = 0;
  q->total_enqueued = 0;
  q->acceptor_blocked = 0;
  pthread_mutex_init(&q->lock, NULL);
  pthread_cond_init(&q->not_empty, NULL);
  pthread_cond_init(&q->not_full, NULL);
}

// Blocks while the queue is full. Blocking the acceptor is deliberate: it lets
// the listen backlog absorb the overflow and, once that fills, the kernel
// refuses new connections. Dropping the fd instead would hide the overload.
static void queue_push(fd_queue_t* q, int fd) {
  pthread_mutex_lock(&q->lock);
  while (q->count == q->capacity) {
    q->acceptor_blocked++;
    pthread_cond_wait(&q->not_full, &q->lock);
  }
  q->fds[q->tail] = fd;
  q->tail = (q->tail + 1) % q->capacity;
  q->count++;
  q->total_enqueued++;
  pthread_mutex_unlock(&q->lock);
  pthread_cond_signal(&q->not_empty);
}

static int queue_pop(fd_queue_t* q) {
  pthread_mutex_lock(&q->lock);
  while (q->count == 0) {
    pthread_cond_wait(&q->not_empty, &q->lock);
  }
  int fd = q->fds[q->head];
  q->head = (q->head + 1) % q->capacity;
  q->count--;
  pthread_mutex_unlock(&q->lock);
  pthread_cond_signal(&q->not_full);
  return fd;
}

static fd_queue_t queue;

static void* worker_main(void* arg) {
  long id = (long)arg;
  while (1) {
    int sockfd = queue_pop(&queue);
    serve_result_t r = serve_connection(sockfd);
    if (r != SERVE_OK) {
      printf("worker %ld: socket %d finished with %s\n", id, sockfd,
             serve_result_str(r));
    }
    close(sockfd);
  }
  return NULL;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ignore_sigpipe();

  int portnum = DEFAULT_PORT;
  int num_workers = DEFAULT_WORKERS;
  int queue_depth = DEFAULT_QUEUE_DEPTH;
  if (argc >= 2) {
    portnum = atoi(argv[1]);
  }
  if (argc >= 3) {
    num_workers = atoi(argv[2]);
  }
  if (argc >= 4) {
    queue_depth = atoi(argv[3]);
  }
  if (num_workers < 1 || queue_depth < 1) {
    die("num_workers and queue_depth must be at least 1");
  }

  printf("Serving on port %d with %d workers, queue depth %d\n", portnum,
         num_workers, queue_depth);

  queue_init(&queue, (size_t)queue_depth);

  pthread_t* workers = xmalloc((size_t)num_workers * sizeof(pthread_t));
  for (long i = 0; i < num_workers; i++) {
    int rc = pthread_create(&workers[i], NULL, worker_main, (void*)i);
    if (rc != 0) {
      die("pthread_create failed for worker %ld", i);
    }
  }

  int listener = listen_inet_socket(portnum);

  while (1) {
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    int newsockfd =
        accept(listener, (struct sockaddr*)&peer_addr, &peer_addr_len);
    if (newsockfd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      perror_die("accept");
    }
    queue_push(&queue, newsockfd);
  }

  return 0;
}
