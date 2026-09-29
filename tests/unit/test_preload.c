/* Preload smoke test: run with LD_PRELOAD=libnwpad.so. The fake SDL is linked
 * into this executable, as in the game, so this checks the jump-table hooks,
 * pass-through to the real SDL, and controller-event filtering. With
 * NWPAD_SOCKET=1 it also checks the control socket against a running frame loop. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "../../src/hook/sdl_min.h"

extern int fake_swap_calls, fake_dynapi_fills;
void SDL_GL_SwapWindow(SDL_Window *w);
int SDL_PollEvent(SDL_Event *e);

static atomic_bool client_done;
static char replies[2][1024];

/* Send ping and status over the control socket; one reply line each. */
static void *socket_client(void *arg) {
    (void)arg;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s/nwpad.sock", getenv("XDG_RUNTIME_DIR"));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        const char *req = "{\"cmd\":\"ping\"}\n{\"cmd\": \"status\"}\n";
        if (write(fd, req, strlen(req)) == (ssize_t)strlen(req)) {
            char buf[1024];
            size_t len = 0;
            ssize_t r;
            while (len < sizeof buf - 1 && (r = read(fd, buf + len, sizeof buf - 1 - len)) > 0) {
                len += (size_t)r;
                buf[len] = '\0';
                char *nl = strchr(buf, '\n');
                if (nl && strchr(nl + 1, '\n')) break;
            }
            buf[len] = '\0';
            char *second = strchr(buf, '\n');
            if (second) {
                *second++ = '\0';
                snprintf(replies[0], sizeof replies[0], "%s", buf);
                char *end = strchr(second, '\n');
                if (end) *end = '\0';
                snprintf(replies[1], sizeof replies[1], "%s", second);
            }
        }
    }
    if (fd >= 0) close(fd);
    atomic_store(&client_done, true);
    return NULL;
}

static int test_socket(void) {
    int fails = 0;
    pthread_t t;
    pthread_create(&t, NULL, socket_client, NULL);
    for (int i = 0; i < 3000 && !atomic_load(&client_done); i++) {
        SDL_GL_SwapWindow(NULL); /* the frame loop answers requests */
        usleep(1000);
    }
    pthread_join(t, NULL);
    if (!strstr(replies[0], "\"ok\":true") || !strstr(replies[0], "\"frame\":")) {
        fprintf(stderr, "FAIL: bad ping reply: %s\n", replies[0]); fails++;
    }
    if (!strstr(replies[1], "\"ok\":true") || !strstr(replies[1], "\"hooks\":true")) {
        fprintf(stderr, "FAIL: bad status reply: %s\n", replies[1]); fails++;
    }
    return fails;
}

int main(void) {
    int fails = 0, seen = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        seen++;
        if (e.type >= SDL_CONTROLLER_FIRST && e.type <= SDL_CONTROLLER_LAST) {
            fprintf(stderr, "FAIL: controller event leaked through\n");
            fails++;
        }
    }
    if (seen != 2) { fprintf(stderr, "FAIL: expected 2 events, saw %d\n", seen); fails++; }
    for (int i = 0; i < 3; i++) SDL_GL_SwapWindow(NULL);
    if (fake_swap_calls != 3) { fprintf(stderr, "FAIL: swap not forwarded\n"); fails++; }
    if (fake_dynapi_fills != 1) { fprintf(stderr, "FAIL: jump table filled %d times\n", fake_dynapi_fills); fails++; }
    const char *sock = getenv("NWPAD_SOCKET");
    if (sock && strcmp(sock, "1") == 0) fails += test_socket();
    printf("preload smoke: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
