// Load generator for the servers in src/.
//
// Opens N concurrent connections, sends M framed messages down each one, and
// records the round-trip latency of every request. Verifies each echo against
// the expected transform, so a server that is fast and wrong fails the run
// rather than posting a good number.
//
// Usage:
//   loadgen <host> <port> <connections> <messages> [payload_bytes] [csv_path]
// Defaults: payload_bytes 32, csv_path none (summary to stdout only)
//
// Output: p50/p90/p99/p99.9 latency in microseconds, throughput in requests per
// second, and error counts. With a csv_path, every individual latency is
// written for offline analysis.

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_PAYLOAD 512

// A connection that never completes must fail rather than stall the whole
// sweep. Both apply per syscall, not per connection.
#define CONNECT_TIMEOUT_S 5
#define IO_TIMEOUT_S 10

typedef struct {
  int id;
  const char* host;
  const char* port;
  int messages;
  int payload_bytes;

  // Filled in by the thread.
  double connect_us;     // TCP handshake only
  double ack_us;         // wait for the server's '*', i.e. time to first byte
  int connected;         // 1 if the connection was established
  double* latencies_us;  // one per successful request
  int completed;
  int errors;
  int mismatches;
} conn_ctx_t;

// Every thread connects, then waits here. The request clock starts only once
// all of them are through, so connection setup cannot leak into the throughput
// figure. Without this, an accept-queue overflow adds a whole second of SYN
// retransmit to wall time and looks like a throughput collapse.
static pthread_barrier_t start_barrier;

static double now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

// connect() with a bounded wait. A blocking connect against a server whose
// accept queue is full can hang for well over a minute while the kernel retries
// SYNs, which stalls the entire sweep on one saturated data point.
static int connect_with_timeout(int fd, const struct sockaddr* addr,
                                socklen_t addrlen, int seconds) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -1;
  }

  int rc = connect(fd, addr, addrlen);
  if (rc < 0 && errno != EINPROGRESS) {
    return -1;
  }

  if (rc < 0) {
    // poll() rather than select(): above 1024 connections the file descriptors
    // exceed FD_SETSIZE and FD_SET writes off the end of the fd_set. The tool
    // built to measure select()'s limit must not inherit it.
    struct pollfd pfd = {.fd = fd, .events = POLLOUT, .revents = 0};
    rc = poll(&pfd, 1, seconds * 1000);
    if (rc <= 0) {
      errno = (rc == 0) ? ETIMEDOUT : errno;
      return -1;
    }

    // select() reporting writable does not mean the connect succeeded.
    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0 || soerr != 0) {
      errno = soerr ? soerr : errno;
      return -1;
    }
  }

  // Back to blocking; SO_RCVTIMEO and SO_SNDTIMEO bound the reads and writes.
  return fcntl(fd, F_SETFL, flags);
}

static int connect_to(const char* host, const char* port) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res;
  if (getaddrinfo(host, port, &hints, &res) != 0) {
    return -1;
  }

  int fd = -1;
  for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (connect_with_timeout(fd, p->ai_addr, p->ai_addrlen,
                             CONNECT_TIMEOUT_S) == 0) {
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);

  if (fd >= 0) {
    int one = 1;
    // Without this, Nagle batches small writes and every latency number
    // measures the 40ms delayed-ack timer instead of the server.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // A server that accepts and then never replies must not hang the sweep.
    struct timeval tv = {.tv_sec = IO_TIMEOUT_S, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  }
  return fd;
}

// Reads exactly len bytes, retrying on partial reads and EINTR.
static int recv_exact(int fd, uint8_t* buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = recv(fd, buf + got, len - got, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      // EAGAIN here is SO_RCVTIMEO firing: the server went quiet.
      return -1;
    }
    if (n == 0) {
      return -1;  // peer closed early
    }
    got += (size_t)n;
  }
  return 0;
}

static int send_all_fd(int fd, const uint8_t* buf, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, buf + sent, len - sent, 0);
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

static void* conn_main(void* arg) {
  conn_ctx_t* ctx = (conn_ctx_t*)arg;

  double c0 = now_us();
  int fd = connect_to(ctx->host, ctx->port);
  ctx->connect_us = now_us() - c0;
  ctx->connected = (fd >= 0);

  // The barrier sits here, after the TCP handshake and before the server's
  // ack. It cannot wait for the ack: a thread pool server sends '*' only once a
  // worker picks the connection up, and a worker is not free until an earlier
  // client finishes. Waiting for all acks before releasing anyone deadlocks the
  // run against the server's own queueing.
  pthread_barrier_wait(&start_barrier);

  if (fd < 0) {
    ctx->errors = ctx->messages;
    return NULL;
  }

  // Time to first byte. For an accept-and-serve-immediately server this is
  // microseconds; for a queueing server it is however long the connection
  // waited for a worker, which is the number worth seeing.
  double a0 = now_us();
  uint8_t ack;
  if (recv_exact(fd, &ack, 1) < 0 || ack != '*') {
    ctx->errors = ctx->messages;
    close(fd);
    return NULL;
  }
  ctx->ack_us = now_us() - a0;

  int n = ctx->payload_bytes;
  uint8_t req[MAX_PAYLOAD + 2];
  uint8_t expected[MAX_PAYLOAD];
  uint8_t got[MAX_PAYLOAD];

  req[0] = '^';
  for (int i = 0; i < n; i++) {
    // Printable range, avoiding '^' and '$' so the payload cannot reframe.
    uint8_t c = (uint8_t)('a' + (i % 26));
    req[1 + i] = c;
    expected[i] = c + 1;
  }
  req[1 + n] = '$';

  for (int m = 0; m < ctx->messages; m++) {
    double t0 = now_us();

    if (send_all_fd(fd, req, (size_t)n + 2) < 0) {
      ctx->errors++;
      break;
    }
    if (recv_exact(fd, got, (size_t)n) < 0) {
      ctx->errors++;
      break;
    }

    double t1 = now_us();

    if (memcmp(got, expected, (size_t)n) != 0) {
      ctx->mismatches++;
    }
    ctx->latencies_us[ctx->completed++] = t1 - t0;
  }

  close(fd);
  return NULL;
}

static int cmp_double(const void* a, const void* b) {
  double x = *(const double*)a;
  double y = *(const double*)b;
  return (x > y) - (x < y);
}

static double percentile(const double* sorted, int n, double p) {
  if (n == 0) {
    return 0.0;
  }
  double idx = p / 100.0 * (double)(n - 1);
  int lo = (int)idx;
  int hi = lo + 1 >= n ? n - 1 : lo + 1;
  double frac = idx - (double)lo;
  return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr,
            "usage: %s <host> <port> <connections> <messages> "
            "[payload_bytes] [csv_path]\n",
            argv[0]);
    return 2;
  }

  const char* host = argv[1];
  const char* port = argv[2];
  int connections = atoi(argv[3]);
  int messages = atoi(argv[4]);
  int payload = argc >= 6 ? atoi(argv[5]) : 32;
  const char* csv_path = argc >= 7 ? argv[6] : NULL;

  if (connections < 1 || messages < 1 || payload < 1 || payload > MAX_PAYLOAD) {
    fprintf(stderr, "bad arguments\n");
    return 2;
  }

  conn_ctx_t* ctxs = calloc((size_t)connections, sizeof(conn_ctx_t));
  pthread_t* threads = calloc((size_t)connections, sizeof(pthread_t));
  if (!ctxs || !threads) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }

  for (int i = 0; i < connections; i++) {
    ctxs[i].id = i;
    ctxs[i].host = host;
    ctxs[i].port = port;
    ctxs[i].messages = messages;
    ctxs[i].payload_bytes = payload;
    ctxs[i].latencies_us = calloc((size_t)messages, sizeof(double));
    if (!ctxs[i].latencies_us) {
      fprintf(stderr, "out of memory\n");
      return 1;
    }
  }

  // +1 for this thread, which releases the workers once all have connected.
  pthread_barrier_init(&start_barrier, NULL, (unsigned)connections + 1);

  double setup_start = now_us();
  int spawned = 0;
  for (int i = 0; i < connections; i++) {
    if (pthread_create(&threads[i], NULL, conn_main, &ctxs[i]) != 0) {
      fprintf(stderr, "pthread_create failed at connection %d\n", i);
      break;
    }
    spawned++;
  }

  if (spawned < connections) {
    // Threads that never started will never reach the barrier. Lower the count
    // by waiting once per missing thread so the rest are released.
    for (int i = spawned; i < connections; i++) {
      pthread_barrier_wait(&start_barrier);
    }
    connections = spawned;
  }

  pthread_barrier_wait(&start_barrier);
  double setup_us = now_us() - setup_start;
  double wall_start = now_us();

  for (int i = 0; i < spawned; i++) {
    pthread_join(threads[i], NULL);
  }

  double wall_us = now_us() - wall_start;

  int total = 0, errors = 0, mismatches = 0;
  for (int i = 0; i < connections; i++) {
    total += ctxs[i].completed;
    errors += ctxs[i].errors;
    mismatches += ctxs[i].mismatches;
  }

  double* all = calloc((size_t)(total > 0 ? total : 1), sizeof(double));
  int k = 0;
  for (int i = 0; i < connections; i++) {
    for (int j = 0; j < ctxs[i].completed; j++) {
      all[k++] = ctxs[i].latencies_us[j];
    }
  }
  qsort(all, (size_t)total, sizeof(double), cmp_double);

  double mean = 0.0;
  for (int i = 0; i < total; i++) {
    mean += all[i];
  }
  if (total > 0) {
    mean /= (double)total;
  }

  int established = 0;
  double connect_max = 0.0, connect_sum = 0.0, ack_max = 0.0;
  for (int i = 0; i < connections; i++) {
    established += ctxs[i].connected;
    connect_sum += ctxs[i].connect_us;
    if (ctxs[i].connect_us > connect_max) {
      connect_max = ctxs[i].connect_us;
    }
    if (ctxs[i].ack_us > ack_max) {
      ack_max = ctxs[i].ack_us;
    }
  }

  printf("connections    %d\n", connections);
  printf("established    %d\n", established);
  printf("setup_ms       %.1f\n", setup_us / 1000.0);
  printf("connect_mean_us %.1f\n",
         connections > 0 ? connect_sum / connections : 0.0);
  printf("connect_max_us %.1f\n", connect_max);
  printf("ack_max_us     %.1f\n", ack_max);
  printf("requests       %d\n", total);
  printf("errors         %d\n", errors);
  printf("mismatches     %d\n", mismatches);
  printf("wall_ms        %.1f\n", wall_us / 1000.0);
  printf("throughput_rps %.0f\n",
         wall_us > 0 ? (double)total / (wall_us / 1e6) : 0.0);
  printf("mean_us        %.1f\n", mean);
  printf("p50_us         %.1f\n", percentile(all, total, 50.0));
  printf("p90_us         %.1f\n", percentile(all, total, 90.0));
  printf("p99_us         %.1f\n", percentile(all, total, 99.0));
  printf("p999_us        %.1f\n", percentile(all, total, 99.9));
  printf("max_us         %.1f\n", total > 0 ? all[total - 1] : 0.0);

  if (csv_path) {
    FILE* f = fopen(csv_path, "w");
    if (!f) {
      perror("fopen");
    } else {
      fprintf(f, "latency_us\n");
      for (int i = 0; i < total; i++) {
        fprintf(f, "%.3f\n", all[i]);
      }
      fclose(f);
    }
  }

  // Wrong answers invalidate the timing, so fail the run.
  return (errors > 0 || mismatches > 0) ? 1 : 0;
}
