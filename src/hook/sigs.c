/* Signature resolver. See sigs.h. */
#define _GNU_SOURCE
#include "sigs.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../core/nwpad_core.h"

#ifdef __linux__
#include <link.h>
#endif

static void *resolved[NWPAD_SIG_COUNT > 0 ? NWPAD_SIG_COUNT : 1];
static int resolved_count;

#ifdef __linux__
/* The main program is the first object dl_iterate_phdr reports. */
static struct {
    bool found;
    uintptr_t base;
    const uint8_t *text; /* the executable PT_LOAD segment */
    size_t text_len;
} exe;

static int find_exe(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size; (void)data;
    exe.found = true;
    exe.base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
            exe.text = (const uint8_t *)(info->dlpi_addr + ph->p_vaddr);
            exe.text_len = ph->p_memsz;
            break;
        }
    }
    return 1; /* stop after the first object */
}
#endif

/* Only accept symbols defined by the game itself, not by some other library. */
static bool in_main_program(void *addr) {
#ifdef __linux__
    Dl_info info;
    return dladdr(addr, &info) && (uintptr_t)info.dli_fbase == exe.base;
#else
    (void)addr;
    return true;
#endif
}

static void *resolve_one(const nwpad_sig_def *d) {
    if (d->symbol) {
        void *p = dlsym(RTLD_DEFAULT, d->symbol);
        if (!p) {
            fprintf(stderr, "[nwpad] signature %s: symbol %s not found\n", d->key, d->symbol);
            return NULL;
        }
        if (!in_main_program(p)) {
            fprintf(stderr, "[nwpad] signature %s: %s is not defined by the game\n", d->key,
                    d->symbol);
            return NULL;
        }
        return p;
    }
    nwpad_pattern pat;
    if (!d->pattern || !nwpad_pattern_parse(d->pattern, &pat)) {
        fprintf(stderr, "[nwpad] signature %s: invalid pattern\n", d->key);
        return NULL;
    }
#ifdef __linux__
    if (!exe.text) {
        fprintf(stderr, "[nwpad] signature %s: game code segment not found\n", d->key);
        return NULL;
    }
    int count;
    const uint8_t *hit = nwpad_pattern_find(exe.text, exe.text_len, &pat, &count);
    if (count != d->expect || !hit) {
        fprintf(stderr, "[nwpad] signature %s: expected %d match(es), found %d\n", d->key,
                d->expect, count);
        return NULL;
    }
    return (void *)(uintptr_t)(hit + d->offset);
#else
    fprintf(stderr, "[nwpad] signature %s: pattern scanning is Linux-only\n", d->key);
    return NULL;
#endif
}

void nwpad_sigs_resolve(void) {
#ifdef __linux__
    dl_iterate_phdr(find_exe, NULL);
#endif
    resolved_count = 0;
    for (int i = 0; i < NWPAD_SIG_COUNT; i++) {
        resolved[i] = resolve_one(&nwpad_sig_defs[i]);
        if (resolved[i]) resolved_count++;
    }
    fprintf(stderr, "[nwpad] signatures: %d/%d resolved\n", resolved_count, NWPAD_SIG_COUNT);
}

void *nwpad_sig(int id) {
    return id >= 0 && id < NWPAD_SIG_COUNT ? resolved[id] : NULL;
}

bool nwpad_sigs_all(const int *ids, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!nwpad_sig(ids[i])) return false;
    return true;
}

void nwpad_sigs_json(char *out, size_t cap) {
    int n = snprintf(out, cap, "{\"resolved\":%d,\"total\":%d,\"missing\":[", resolved_count,
                     NWPAD_SIG_COUNT);
    bool first = true;
    for (int i = 0; i < NWPAD_SIG_COUNT && n > 0 && (size_t)n < cap; i++) {
        if (resolved[i]) continue;
        n += snprintf(out + n, cap - (size_t)n, "%s\"%s\"", first ? "" : ",",
                      nwpad_sig_defs[i].key);
        first = false;
    }
    if (n > 0 && (size_t)n < cap) snprintf(out + n, cap - (size_t)n, "]}");
}
