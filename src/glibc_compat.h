/* Pin glibc symbol versions to the x86-64 baseline so the library loads on
 * older systems (the Steam Deck, the Steam Runtime) than the one it's built
 * on. Newer glibc re-versioned these (atan2f 2.43, fmodf 2.38, dlsym/dladdr
 * 2.34); the baseline versions still exist everywhere. Force-included by
 * CMake on Linux x86-64 only. */
#ifndef NWPAD_GLIBC_COMPAT_H
#define NWPAD_GLIBC_COMPAT_H
#if defined(__linux__) && defined(__x86_64__)
__asm__(".symver atan2f,atan2f@GLIBC_2.2.5");
__asm__(".symver fmodf,fmodf@GLIBC_2.2.5");
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
__asm__(".symver dladdr,dladdr@GLIBC_2.2.5");
#endif
#endif
