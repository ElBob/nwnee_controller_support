/* Crash reporting for field testing: on a fatal signal, log the signal, the
 * faulting address, the instruction pointer, what the library was doing
 * (nwpad_where), and a backtrace to stderr, then hand the signal on to the
 * handler that was installed before (the game's own). Built with
 * NWPAD_CRASH_TRACE (default on, Linux only). */
#ifndef NWPAD_CRASHTRACE_H
#define NWPAD_CRASHTRACE_H

/* A static string naming the library's current step; "" when idle. */
extern const char *volatile nwpad_where;

#ifdef NWPAD_CRASH_TRACE
/* Install after the game has registered its own crash handlers. */
void nwpad_crashtrace_install(void);
#define NWPAD_WHERE(s) (nwpad_where = (s))
#else
static inline void nwpad_crashtrace_install(void) {}
#define NWPAD_WHERE(s) ((void)0)
#endif

#endif
