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
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_PAYLOAD 512

typedef struct {
  int id;
  const char* host;
  const char* port;
  int messages;
  int payload_bytes;

  // Filled in by the thread.
  double* latencies_us;  // one per successful request
  int completed;
  int errors;
  int mismatches;
} conn_ctx_t;

static double now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
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
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
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

  int fd = connect_to(ctx->host, ctx->port);
  if (fd < 0) {
    ctx->errors = ctx->messages;
    return NULL;
  }

  // Every server sends '*' on connect. Consume it before timing anything, or
  // the first request absorbs the handshake.
  uint8_t ack;
  if (recv_exact(fd, &ack, 1) < 0 || ack != '*') {
    ctx->errors = ctx->messages;
    close(fd);
    return NULL;
  }

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

  double wall_start = now_us();

  for (int i = 0; i < connections; i++) {
    if (pthread_create(&threads[i], NULL, conn_main, &ctxs[i]) != 0) {
      fprintf(stderr, "pthread_create failed at connection %d\n", i);
      connections = i;
      break;
    }
  }
  for (int i = 0; i < connections; i++) {
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

  printf("connections    %d\n", connections);
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
