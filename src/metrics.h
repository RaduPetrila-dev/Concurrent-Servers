#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Process-wide metric registry rendered in the Prometheus text exposition
 * format. All mutators are safe to call from any thread and never allocate.
 * Every series carries a model="<name>" label so several server models can be
 * scraped into one dashboard.
 */

void metrics_init(const char *model);

void metrics_connection_accepted(void);
void metrics_connection_refused(void);
void metrics_connection_closed(void);

void metrics_request_observed(uint64_t duration_ns);

void metrics_queue_depth_set(uint64_t depth);
void metrics_workers_busy_add(int64_t delta);

/*
 * Writes the exposition body into out. Returns the length the body would have
 * had, so a return value >= cap means the output was truncated.
 */
size_t metrics_render(char *out, size_t cap);

uint64_t metrics_now_ns(void);

#endif /* METRICS_H */
