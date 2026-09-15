// Thread pool socket server.
//
// A fixed number of worker threads pull accepted connections off a bounded
// queue. This removes the thread-per-connection ceiling in threaded_server.c:
// memory and scheduler cost stop growing with the number of clients, and the
// queue provides backpressure when clients arrive faster than workers finish.
//
// Usage: threadpool_server [port] [num_workers] [queue_depth] [metrics_port]
// Defaults:                9090   4             64            9110
//
// The metrics port answers GET /metrics in the Prometheus text exposition
// format. METRICS_PORT in the environment sets the same value; an explicit
// fourth argument wins over it. Either set to 0 turns the endpoint off, which
// is what a benchmark sweep wants when several servers run back to back.

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "metrics.h"
#include "metrics_server.h"
#include "protocol.h"
#include "utils.h"

#define DEFAULT_PORT 9090
#define DEFAULT_WORKERS 4
#define DEFAULT_QUEUE_DEPTH 64
#define DEFAULT_METRICS_PORT 9110

// Pause after an accept() that failed on a resource limit. Without it the loop
// spins on a call that cannot succeed until a descriptor frees up.
#define ACCEPT_BACKOFF_MS 10

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

// Set from a signal handler so the accept loop can finish its current iteration
// and shut the metrics thread down, rather than dying mid-write.
static volatile sig_atomic_t stop_requested;

static void on_stop_signal(int sig) {
  (void)sig;
  stop_requested = 1;
}

// SA_RESTART is deliberately left off. accept() has to return EINTR for the
// loop to notice stop_requested; with the flag set it would resume blocking and
// the server would ignore the signal.
static void install_stop_handler(void) {
  struct sigaction sa;

  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_stop_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
    perror_die("sigaction");
  }
}

// atoi cannot tell 0 from a malformed argument, which matters here because 0 is
// a meaningful metrics port. strtol can, so a typo fails loudly at startup
// instead of silently disabling the endpoint.
static int parse_port(const char* text, int fallback) {
  char* end = NULL;
  long value;

  if (text == NULL || text[0] == '\0') {
    return fallback;
  }

  errno = 0;
  value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 65535) {
    die("port must be an integer in 0..65535, got \"%s\"", text);
  }
  return (int)value;
}

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
  metrics_queue_depth_set((uint64_t)q->count);
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
  metrics_queue_depth_set((uint64_t)q->count);
  pthread_mutex_unlock(&q->lock);
  pthread_cond_signal(&q->not_full);
  return fd;
}

static fd_queue_t queue;

static void* worker_main(void* arg) {
  long id = (long)arg;
  while (1) {
    int sockfd = queue_pop(&queue);

    // The busy gauge brackets serve_connection only. Time spent waiting in
    // queue_pop is idle time, and counting it would make a starved pool look
    // saturated.
    metrics_workers_busy_add(1);
    serve_result_t r = serve_connection(sockfd);
    metrics_workers_busy_add(-1);

    if (r != SERVE_OK) {
      printf("worker %ld: socket %d finished with %s\n", id, sockfd,
             serve_result_str(r));
    }
    close(sockfd);
    metrics_connection_closed();
  }
  return NULL;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  ignore_sigpipe();
  install_stop_handler();

  int portnum = DEFAULT_PORT;
  int num_workers = DEFAULT_WORKERS;
  int queue_depth = DEFAULT_QUEUE_DEPTH;
  int metrics_port = parse_port(getenv("METRICS_PORT"), DEFAULT_METRICS_PORT);

  if (argc >= 2) {
    portnum = parse_port(argv[1], DEFAULT_PORT);
  }
  if (argc >= 3) {
    num_workers = atoi(argv[2]);
  }
  if (argc >= 4) {
    queue_depth = atoi(argv[3]);
  }
  if (argc >= 5) {
    metrics_port = parse_port(argv[4], DEFAULT_METRICS_PORT);
  }
  if (num_workers < 1 || queue_depth < 1) {
    die("num_workers and queue_depth must be at least 1");
  }

  // Before the endpoint opens, so a scrape can never read an unset model label.
  metrics_init("threadpool");

  printf("Serving on port %d with %d workers, queue depth %d\n", portnum,
         num_workers, queue_depth);

  if (metrics_port > 0) {
    if (metrics_server_start((uint16_t)metrics_port) != 0) {
      perror_die("metrics_server_start");
    }
    printf("Metrics on http://127.0.0.1:%d/metrics\n", metrics_port);
  } else {
    printf("Metrics endpoint disabled\n");
  }

  queue_init(&queue, (size_t)queue_depth);

  pthread_t* workers = xmalloc((size_t)num_workers * sizeof(pthread_t));
  for (long i = 0; i < num_workers; i++) {
    int rc = pthread_create(&workers[i], NULL, worker_main, (void*)i);
    if (rc != 0) {
      die("pthread_create failed for worker %ld", i);
    }
  }

  int listener = listen_inet_socket(portnum);

  while (!stop_requested) {
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    int newsockfd =
        accept(listener, (struct sockaddr*)&peer_addr, &peer_addr_len);
    if (newsockfd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) {
        continue;
      }
      // The peer finished its handshake and is then dropped because this
      // process has no descriptor for it. That is a refusal the server owns,
      // unlike a backlog overflow, which the kernel counts in ListenOverflows.
      if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS ||
          errno == ENOMEM) {
        metrics_connection_refused();
        (void)poll(NULL, 0, ACCEPT_BACKOFF_MS);
        continue;
      }
      perror_die("accept");
    }
    metrics_connection_accepted();
    queue_push(&queue, newsockfd);
  }

  printf("Stop requested, shutting down\n");
  metrics_server_stop();
  close(listener);
  free(workers);
  return 0;
}
