#define _POSIX_C_SOURCE 200809L

#include "metrics_server.h"

#include "metrics.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define REQUEST_MAX 2048
#define BODY_MAX 16384
#define ACCEPT_POLL_MS 200
#define CLIENT_TIMEOUT_MS 2000

static pthread_t g_thread;
static int g_listen_fd = -1;
static atomic_bool g_running;
static bool g_started;

static int write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
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

static int read_request(int fd, char *buf, size_t cap)
{
    size_t len = 0;

    while (len + 1 < cap) {
        ssize_t n = recv(fd, buf + len, cap - len - 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n") != NULL || strstr(buf, "\n\n") != NULL) {
            return 0;
        }
    }

    buf[len] = '\0';
    return len > 0 ? 0 : -1;
}

static void send_status(int fd, const char *status, const char *body)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, strlen(body));

    if (n > 0 && (size_t)n < sizeof head && write_all(fd, head, (size_t)n) == 0) {
        (void)write_all(fd, body, strlen(body));
    }
}

static void send_metrics(int fd)
{
    char body[BODY_MAX];
    char head[256];
    size_t body_len = metrics_render(body, sizeof body);
    int n;

    if (body_len >= sizeof body) {
        send_status(fd, "500 Internal Server Error", "metrics buffer too small\n");
        return;
    }

    n = snprintf(head, sizeof head,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 body_len);

    if (n > 0 && (size_t)n < sizeof head && write_all(fd, head, (size_t)n) == 0) {
        (void)write_all(fd, body, body_len);
    }
}

static void serve(int fd)
{
    struct timeval tv = {.tv_sec = CLIENT_TIMEOUT_MS / 1000,
                         .tv_usec = (CLIENT_TIMEOUT_MS % 1000) * 1000};
    char request[REQUEST_MAX];

    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    if (read_request(fd, request, sizeof request) != 0) {
        return;
    }

    if (strncmp(request, "GET /metrics", 12) == 0) {
        send_metrics(fd);
    } else {
        send_status(fd, "404 Not Found", "not found\n");
    }

    (void)shutdown(fd, SHUT_WR);
}

static void *accept_loop(void *arg)
{
    (void)arg;

    while (atomic_load_explicit(&g_running, memory_order_relaxed)) {
        struct pollfd pfd = {.fd = g_listen_fd, .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, ACCEPT_POLL_MS);
        int fd;

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            continue;
        }

        fd = accept(g_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED || errno == EMFILE) {
                continue;
            }
            break;
        }

        serve(fd);
        (void)close(fd);
    }

    return NULL;
}

static int open_listener(uint16_t port)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;

    if (fd < 0) {
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
        goto fail;
    }

    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        goto fail;
    }
    if (listen(fd, 16) != 0) {
        goto fail;
    }

    return fd;

fail: {
    int saved = errno;
    (void)close(fd);
    errno = saved;
    return -1;
}
}

int metrics_server_start(uint16_t port)
{
    int rc;

    if (g_started) {
        errno = EALREADY;
        return -1;
    }

    g_listen_fd = open_listener(port);
    if (g_listen_fd < 0) {
        return -1;
    }

    atomic_store_explicit(&g_running, true, memory_order_relaxed);

    rc = pthread_create(&g_thread, NULL, accept_loop, NULL);
    if (rc != 0) {
        atomic_store_explicit(&g_running, false, memory_order_relaxed);
        (void)close(g_listen_fd);
        g_listen_fd = -1;
        errno = rc;
        return -1;
    }

    g_started = true;
    return 0;
}

void metrics_server_stop(void)
{
    if (!g_started) {
        return;
    }

    atomic_store_explicit(&g_running, false, memory_order_relaxed);
    (void)pthread_join(g_thread, NULL);
    (void)close(g_listen_fd);

    g_listen_fd = -1;
    g_started = false;
}
