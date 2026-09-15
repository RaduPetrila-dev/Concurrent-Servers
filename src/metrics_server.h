#ifndef METRICS_SERVER_H
#define METRICS_SERVER_H

#include <stdint.h>

/*
 * Serves the metric registry over HTTP on a background thread.
 * Only GET /metrics is answered; anything else returns 404.
 * Returns 0 on success, -1 on failure with errno set.
 */
int metrics_server_start(uint16_t port);

void metrics_server_stop(void);

#endif /* METRICS_SERVER_H */
