#define _POSIX_C_SOURCE 200809L

#include "metrics.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RENDER_MAX 16384

static void render(char* buf, size_t cap) {
  size_t len = metrics_render(buf, cap);
  assert(len < cap);
}

static int line_value(const char* body, const char* prefix,
                      unsigned long long* out) {
  const char* at = strstr(body, prefix);
  const char* space;

  if (at == NULL) {
    return -1;
  }
  space = strchr(at, ' ');
  if (space == NULL) {
    return -1;
  }
  *out = strtoull(space + 1, NULL, 10);
  return 0;
}

static void test_model_label_is_sanitised(void) {
  char body[RENDER_MAX];

  metrics_init("thread pool");
  render(body, sizeof body);
  assert(strstr(body, "model=\"unknown\"") != NULL);

  metrics_init("threadpool");
  render(body, sizeof body);
  assert(strstr(body, "model=\"threadpool\"") != NULL);
}

static void test_render_without_init_is_still_valid(void) {
  char body[RENDER_MAX];

  render(body, sizeof body);
  assert(strstr(body, "model=\"\"") == NULL);
  assert(strstr(body, "model=\"unknown\"") != NULL);
}

static void test_counters_and_gauges(void) {
  char body[RENDER_MAX];
  unsigned long long value;

  metrics_init("epoll");

  for (int i = 0; i < 7; i++) {
    metrics_connection_accepted();
  }
  metrics_connection_closed();
  metrics_connection_closed();
  metrics_connection_refused();
  metrics_queue_depth_set(64);
  metrics_workers_busy_add(3);
  metrics_workers_busy_add(-1);

  render(body, sizeof body);

  assert(line_value(body, "server_connections_accepted_total{model=\"epoll\"}",
                    &value) == 0);
  assert(value == 7);
  assert(line_value(body, "server_connections_refused_total{model=\"epoll\"}",
                    &value) == 0);
  assert(value == 1);
  assert(line_value(body, "server_connections_active{model=\"epoll\"}",
                    &value) == 0);
  assert(value == 5);
  assert(line_value(body, "server_pool_queue_depth{model=\"epoll\"}", &value) ==
         0);
  assert(value == 64);
  assert(line_value(body, "server_pool_workers_busy{model=\"epoll\"}",
                    &value) == 0);
  assert(value == 2);
}

static void test_histogram_boundaries_are_inclusive(void) {
  char body[RENDER_MAX];
  unsigned long long value;

  metrics_init("threadpool");

  metrics_request_observed(20000);
  metrics_request_observed(20001);
  metrics_request_observed(60000000);

  render(body, sizeof body);

  assert(line_value(body, "le=\"0.00002\"} ", &value) == 0);
  assert(value == 1);
  assert(line_value(body, "le=\"0.00005\"} ", &value) == 0);
  assert(value == 2);
  assert(line_value(body, "le=\"0.05\"} ", &value) == 0);
  assert(value == 2);
  assert(line_value(body, "le=\"+Inf\"} ", &value) == 0);
  assert(value == 3);
  assert(line_value(body, "server_request_duration_seconds_count", &value) ==
         0);
  assert(value == 3);
  assert(strstr(body,
                "server_request_duration_seconds_sum{model=\"threadpool\"} "
                "0.060040001") != NULL);
}

static void test_buckets_are_monotonic(void) {
  char body[RENDER_MAX];
  const char* cursor;
  unsigned long long previous = 0;

  metrics_init("select");
  for (uint64_t ns = 1000; ns < 100000000; ns *= 3) {
    metrics_request_observed(ns);
  }
  render(body, sizeof body);

  cursor = body;
  while ((cursor = strstr(cursor, "_bucket{")) != NULL) {
    const char* space = strchr(cursor, ' ');
    unsigned long long value;

    assert(space != NULL);
    value = strtoull(space + 1, NULL, 10);
    assert(value >= previous);
    previous = value;
    cursor = space;
  }
  assert(previous > 0);
}

static void test_truncation_reports_required_length(void) {
  char small[64];
  size_t needed;

  metrics_init("sequential");
  metrics_connection_accepted();

  needed = metrics_render(small, sizeof small);
  assert(needed > sizeof small);
  assert(small[sizeof small - 1] == '\0');

  assert(metrics_render(NULL, 0) == needed);
}

#define THREADS 8
#define PER_THREAD 20000

static void* hammer(void* arg) {
  (void)arg;
  for (int i = 0; i < PER_THREAD; i++) {
    metrics_connection_accepted();
    metrics_request_observed((uint64_t)(i % 1000) * 100);
    metrics_connection_closed();
  }
  return NULL;
}

static void test_concurrent_updates_do_not_lose_counts(void) {
  pthread_t threads[THREADS];
  char body[RENDER_MAX];
  unsigned long long value;

  metrics_init("threadpool");

  for (int i = 0; i < THREADS; i++) {
    assert(pthread_create(&threads[i], NULL, hammer, NULL) == 0);
  }
  for (int i = 0; i < THREADS; i++) {
    assert(pthread_join(threads[i], NULL) == 0);
  }

  render(body, sizeof body);

  assert(line_value(body,
                    "server_connections_accepted_total{model=\"threadpool\"}",
                    &value) == 0);
  assert(value == (unsigned long long)THREADS * PER_THREAD);
  assert(line_value(body, "server_connections_active{model=\"threadpool\"}",
                    &value) == 0);
  assert(value == 0);
  assert(line_value(body, "server_request_duration_seconds_count", &value) ==
         0);
  assert(value == (unsigned long long)THREADS * PER_THREAD);
}

static void test_monotonic_clock_advances(void) {
  uint64_t start = metrics_now_ns();
  uint64_t spin = 0;

  for (int i = 0; i < 1000000; i++) {
    spin += (uint64_t)i;
  }
  assert(spin > 0);
  assert(metrics_now_ns() >= start);
}

int main(void) {
  test_render_without_init_is_still_valid();
  test_model_label_is_sanitised();
  test_counters_and_gauges();
  test_histogram_boundaries_are_inclusive();
  test_buckets_are_monotonic();
  test_truncation_reports_required_length();
  test_concurrent_updates_do_not_lose_counts();
  test_monotonic_clock_advances();

  printf("test_metrics: all assertions passed\n");
  return 0;
}
