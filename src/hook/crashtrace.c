/* See crashtrace.h. */
#define _GNU_SOURCE
#include "crashtrace.h"

#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

const char *volatile nwpad_where = "";

static const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
static struct sigaction previous[sizeof signals / sizeof signals[0]];

static void say(const char *s) { (void)!write(STDERR_FILENO, s, strlen(s)); }

static void on_crash(int sig, siginfo_t *info, void *ctx) {
    char buf[256];
    void *pc = NULL;
#if defined(__x86_64__)
    pc = (void *)((ucontext_t *)ctx)->uc_mcontext.gregs[REG_RIP];
#else
    (void)ctx;
#endif
    snprintf(buf, sizeof buf, "[nwpad] CRASH signal %d (%s) at %p, pc %p, while: %s\n", sig,
             strsignal(sig), info ? info->si_addr : NULL, pc, nwpad_where[0] ? nwpad_where : "(not in nwpad)");
    say(buf);
    void *frames[32];
    int n = backtrace(frames, 32);
    say("[nwpad] backtrace:\n");
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    /* Hand on to whatever was there before (the game's crash handler). */
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) {
        if (signals[i] != sig) continue;
        sigaction(sig, &previous[i], NULL);
        if (previous[i].sa_flags & SA_SIGINFO) {
            if (previous[i].sa_sigaction) previous[i].sa_sigaction(sig, info, ctx);
            return;
        }
        if (previous[i].sa_handler == SIG_IGN) return;
        if (previous[i].sa_handler != SIG_DFL) {
            previous[i].sa_handler(sig);
            return;
        }
    }
    raise(sig); /* default action */
}

void nwpad_crashtrace_install(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    void *warm[1];
    backtrace(warm, 1); /* load libgcc's unwinder now, not inside the handler */
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++) sigaction(signals[i], &sa, &previous[i]);
}
