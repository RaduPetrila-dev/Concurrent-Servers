// Demo of dispatching sleeping/blocking in a callback to a work queue.

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <uv.h>

void on_after_work(uv_work_t* req, int status) {
  // The work always succeeds, so there is nothing to report on status.
  (void)status;

  free(req);
}

void on_work(uv_work_t* req) {
  // The request carries no payload; it exists only to occupy a pool thread.
  (void)req;

  // "Work"
  if (random() % 5 == 0) {
    printf("Sleeping...\n");
    sleep(3);
  }
}

void on_timer(uv_timer_t* timer) {
  // Fixed callback signature; the timer handle is not needed here.
  (void)timer;

  uint64_t timestamp = uv_hrtime();
  printf("on_timer [%" PRIu64 " ms]\n", (timestamp / 1000000) % 100000);

  uv_work_t* work_req = (uv_work_t*)malloc(sizeof(*work_req));
  uv_queue_work(uv_default_loop(), work_req, on_work, on_after_work);
}

int main(void) {
  uv_timer_t timer;
  uv_timer_init(uv_default_loop(), &timer);
  uv_timer_start(&timer, on_timer, 0, 1000);
  return uv_run(uv_default_loop(), UV_RUN_DEFAULT);
}
