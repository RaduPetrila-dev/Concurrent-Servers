#define _POSIX_C_SOURCE 200809L

#include "metrics.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define BUCKET_COUNT 10
#define MODEL_MAX 32

static const uint64_t k_bucket_upper_ns[BUCKET_COUNT] = {
    20000,   50000,   100000,  250000,   500000,
    1000000, 2500000, 5000000, 10000000, 50000000,
};

static const char* const k_bucket_le[BUCKET_COUNT] = {
    "0.00002", "0.00005", "0.0001", "0.00025", "0.0005",
    "0.001",   "0.0025",  "0.005",  "0.01",    "0.05",
};

static struct {
  char model[MODEL_MAX];
  atomic_uint_fast64_t accepted;
  atomic_uint_fast64_t refused;
  atomic_int_fast64_t active;
  atomic_int_fast64_t workers_busy;
  atomic_uint_fast64_t queue_depth;
  atomic_uint_fast64_t latency_count[BUCKET_COUNT + 1];
  atomic_uint_fast64_t latency_sum_ns;
} g;

static const char* model_label(void) {
  return g.model[0] != '\0' ? g.model : "unknown";
}

static int is_label_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static void set_model(const char* model) {
  size_t n = 0;

  if (model != NULL) {
    while (model[n] != '\0' && n + 1 < MODEL_MAX) {
      if (!is_label_char(model[n])) {
        n = 0;
        break;
      }
      n++;
    }
  }

  if (n == 0) {
    memcpy(g.model, "unknown", sizeof "unknown");
    return;
  }

  memcpy(g.model, model, n);
  g.model[n] = '\0';
}

void metrics_init(const char* model) {
  set_model(model);

  atomic_store_explicit(&g.accepted, 0, memory_order_relaxed);
  atomic_store_explicit(&g.refused, 0, memory_order_relaxed);
  atomic_store_explicit(&g.active, 0, memory_order_relaxed);
  atomic_store_explicit(&g.workers_busy, 0, memory_order_relaxed);
  atomic_store_explicit(&g.queue_depth, 0, memory_order_relaxed);
  atomic_store_explicit(&g.latency_sum_ns, 0, memory_order_relaxed);

  for (size_t i = 0; i <= BUCKET_COUNT; i++) {
    atomic_store_explicit(&g.latency_count[i], 0, memory_order_relaxed);
  }
}

uint64_t metrics_now_ns(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void metrics_connection_accepted(void) {
  atomic_fetch_add_explicit(&g.accepted, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&g.active, 1, memory_order_relaxed);
}

void metrics_connection_refused(void) {
  atomic_fetch_add_explicit(&g.refused, 1, memory_order_relaxed);
}

void metrics_connection_closed(void) {
  atomic_fetch_sub_explicit(&g.active, 1, memory_order_relaxed);
}

static size_t bucket_index(uint64_t duration_ns) {
  for (size_t i = 0; i < BUCKET_COUNT; i++) {
    if (duration_ns <= k_bucket_upper_ns[i]) {
      return i;
    }
  }
  return BUCKET_COUNT;
}

void metrics_request_observed(uint64_t duration_ns) {
  atomic_fetch_add_explicit(&g.latency_count[bucket_index(duration_ns)], 1,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&g.latency_sum_ns, duration_ns,
                            memory_order_relaxed);
}

void metrics_queue_depth_set(uint64_t depth) {
  atomic_store_explicit(&g.queue_depth, depth, memory_order_relaxed);
}

void metrics_workers_busy_add(int64_t delta) {
  atomic_fetch_add_explicit(&g.workers_busy, delta, memory_order_relaxed);
}

static void appendf(char* out, size_t cap, size_t* len, const char* fmt, ...) {
  char* dst = (*len < cap) ? out + *len : NULL;
  size_t room = (*len < cap) ? cap - *len : 0;
  va_list ap;
  int written;

  va_start(ap, fmt);
  written = vsnprintf(dst, room, fmt, ap);
  va_end(ap);

  if (written > 0) {
    *len += (size_t)written;
  }
}

static void render_counter(char* out, size_t cap, size_t* len, const char* name,
                           const char* help, uint64_t value) {
  appendf(out, cap, len, "# HELP %s %s\n", name, help);
  appendf(out, cap, len, "# TYPE %s counter\n", name);
  appendf(out, cap, len, "%s{model=\"%s\"} %" PRIu64 "\n", name, model_label(),
          value);
}

static void render_gauge(char* out, size_t cap, size_t* len, const char* name,
                         const char* help, int64_t value) {
  appendf(out, cap, len, "# HELP %s %s\n", name, help);
  appendf(out, cap, len, "# TYPE %s gauge\n", name);
  appendf(out, cap, len, "%s{model=\"%s\"} %" PRId64 "\n", name, model_label(),
          value);
}

static void render_latency(char* out, size_t cap, size_t* len) {
  const char* name = "server_request_duration_seconds";
  uint64_t cumulative = 0;
  uint64_t sum_ns;

  appendf(out, cap, len, "# HELP %s Request service time in seconds.\n", name);
  appendf(out, cap, len, "# TYPE %s histogram\n", name);

  for (size_t i = 0; i < BUCKET_COUNT; i++) {
    cumulative +=
        atomic_load_explicit(&g.latency_count[i], memory_order_relaxed);
    appendf(out, cap, len, "%s_bucket{model=\"%s\",le=\"%s\"} %" PRIu64 "\n",
            name, model_label(), k_bucket_le[i], cumulative);
  }

  cumulative += atomic_load_explicit(&g.latency_count[BUCKET_COUNT],
                                     memory_order_relaxed);
  appendf(out, cap, len, "%s_bucket{model=\"%s\",le=\"+Inf\"} %" PRIu64 "\n",
          name, model_label(), cumulative);

  sum_ns = atomic_load_explicit(&g.latency_sum_ns, memory_order_relaxed);
  appendf(out, cap, len, "%s_sum{model=\"%s\"} %" PRIu64 ".%09" PRIu64 "\n",
          name, model_label(), sum_ns / 1000000000ULL, sum_ns % 1000000000ULL);
  appendf(out, cap, len, "%s_count{model=\"%s\"} %" PRIu64 "\n", name,
          model_label(), cumulative);
}

size_t metrics_render(char* out, size_t cap) {
  size_t len = 0;

  if (out == NULL) {
    cap = 0;
  }

  render_counter(out, cap, &len, "server_connections_accepted_total",
                 "Connections accepted since process start.",
                 atomic_load_explicit(&g.accepted, memory_order_relaxed));
  render_counter(out, cap, &len, "server_connections_refused_total",
                 "Connections refused because the server had no capacity.",
                 atomic_load_explicit(&g.refused, memory_order_relaxed));
  render_gauge(out, cap, &len, "server_connections_active",
               "Connections currently open.",
               atomic_load_explicit(&g.active, memory_order_relaxed));
  render_gauge(
      out, cap, &len, "server_pool_queue_depth",
      "Work items waiting in the thread pool queue.",
      (int64_t)atomic_load_explicit(&g.queue_depth, memory_order_relaxed));
  render_gauge(out, cap, &len, "server_pool_workers_busy",
               "Worker threads serving a request.",
               atomic_load_explicit(&g.workers_busy, memory_order_relaxed));
  render_latency(out, cap, &len);

  if (cap > 0 && len >= cap) {
    out[cap - 1] = '\0';
  }

  return len;
}
