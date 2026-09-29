/* Control socket server. See control.h. */
#define _GNU_SOURCE
#include "control.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define REQUEST_MAX 1024
#define REPLY_MAX 4096
#define REPLY_TIMEOUT_MS 2000 /* the main thread must answer within this */

enum { MB_IDLE, MB_PENDING, MB_DONE };

static struct {
    nwpad_control_handler handler;
    pthread_mutex_t lock;
    pthread_cond_t done;
    atomic_int state; /* MB_*; written under lock, read lock-free by the frame hook */
    char request[REQUEST_MAX];
    char reply[REPLY_MAX];
} mb = {.lock = PTHREAD_MUTEX_INITIALIZER, .done = PTHREAD_COND_INITIALIZER};

static int listen_fd = -1;

void nwpad_control_service(void) {
    if (atomic_load_explicit(&mb.state, memory_order_acquire) != MB_PENDING) return;
    if (pthread_mutex_trylock(&mb.lock) != 0) return; /* never block the frame */
    if (atomic_load_explicit(&mb.state, memory_order_relaxed) == MB_PENDING) {
        mb.handler(mb.request, mb.reply, sizeof mb.reply);
        atomic_store_explicit(&mb.state, MB_DONE, memory_order_release);
        pthread_cond_signal(&mb.done);
    }
    pthread_mutex_unlock(&mb.lock);
}

/* Hand one request to the main thread and wait for the reply. */
static void dispatch(const char *request, char *reply, size_t cap) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += REPLY_TIMEOUT_MS / 1000;
    deadline.tv_nsec += (long)(REPLY_TIMEOUT_MS % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&mb.lock);
    snprintf(mb.request, sizeof mb.request, "%s", request);
    atomic_store_explicit(&mb.state, MB_PENDING, memory_order_release);
    int rc = 0;
    while (atomic_load_explicit(&mb.state, memory_order_relaxed) == MB_PENDING && rc == 0)
        rc = pthread_cond_timedwait(&mb.done, &mb.lock, &deadline);
    if (atomic_load_explicit(&mb.state, memory_order_relaxed) == MB_DONE)
        snprintf(reply, cap, "%s", mb.reply);
    else
        snprintf(reply, cap, "{\"ok\":false,\"error\":\"frame loop not running\"}");
    atomic_store_explicit(&mb.state, MB_IDLE, memory_order_relaxed);
    pthread_mutex_unlock(&mb.lock);
}

static bool write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        buf += w;
        n -= (size_t)w;
    }
    return true;
}

/* Serve one client: one JSON object per line, one reply line per request. */
static void serve(int fd) {
    char buf[REQUEST_MAX], reply[REPLY_MAX + 1];
    size_t len = 0;
    for (;;) {
        ssize_t r = read(fd, buf + len, sizeof buf - 1 - len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return;
        len += (size_t)r;
        char *nl;
        while ((nl = memchr(buf, '\n', len)) != NULL) {
            *nl = '\0';
            dispatch(buf, reply, sizeof reply - 1);
            size_t n = strlen(reply);
            reply[n++] = '\n';
            if (!write_all(fd, reply, n)) return;
            len -= (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, len);
        }
        if (len == sizeof buf - 1) return; /* line too long: drop the client */
    }
}

static void *accept_loop(void *arg) {
    (void)arg;
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            return NULL;
        }
        serve(fd);
        close(fd);
    }
}

void nwpad_control_start(nwpad_control_handler handler) {
    const char *on = getenv("NWPAD_SOCKET");
    if (!on || strcmp(on, "1") != 0) return;
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) {
        fprintf(stderr, "[nwpad] XDG_RUNTIME_DIR unset; control socket disabled\n");
        return;
    }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if ((size_t)snprintf(addr.sun_path, sizeof addr.sun_path, "%s/nwpad.sock", dir) >=
        sizeof addr.sun_path) {
        fprintf(stderr, "[nwpad] socket path too long; control socket disabled\n");
        return;
    }
    mb.handler = handler;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) goto fail;
    unlink(addr.sun_path); /* stale socket from an earlier run */
    mode_t old = umask(0177); /* socket file mode 0600 */
    int rc = bind(fd, (struct sockaddr *)&addr, sizeof addr);
    umask(old);
    if (rc != 0 || listen(fd, 4) != 0) goto fail;
    listen_fd = fd;
    pthread_t t;
    if (pthread_create(&t, NULL, accept_loop, NULL) != 0) goto fail;
    pthread_detach(t);
    fprintf(stderr, "[nwpad] control socket %s\n", addr.sun_path);
    return;
fail:
    fprintf(stderr, "[nwpad] control socket setup failed: %s\n", strerror(errno));
    if (fd >= 0) close(fd);
    listen_fd = -1;
}
