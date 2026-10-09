/* See dialog.h. Layout from the dialog plan, first pass (to confirm: F36). */
#define _GNU_SOURCE
#include "dialog.h"

#include "../core/nwpad_core.h"
#include "sigs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

typedef struct { char *ptr; uint32_t len, cap; } exo_string; /* CExoString, 16 bytes */
typedef void *(*vcall_fn)(void *self);

enum { GUI_DIALOG = 0x70 }; /* CGuiInGame -> CGuiInGameChatDialog* */

static char *dialog_object(void) {
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    vcall_fn get_gui = (vcall_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_IN_GAME_GUI);
    if (!app_manager || !*app_manager || !get_gui) return NULL;
    char *gui = get_gui(*(void **)*app_manager);
    return gui ? *(char **)(gui + GUI_DIALOG) : NULL;
}

#ifdef NWPAD_DEBUG_SURFACES
/* Readable without faulting: write() reports EFAULT for unmapped memory. */
static bool readable(const void *p, size_t n) {
    static int fds[2] = {-1, -1};
    if (fds[0] < 0 && pipe(fds) != 0) return false;
    char sink[64];
    if (n > sizeof sink) n = sizeof sink;
    if (write(fds[1], p, n) != (ssize_t)n) return false;
    return read(fds[0], sink, n) == (ssize_t)n;
}

static size_t printable_len(const char *p, size_t max) {
    size_t n = 0;
    while (n < max && readable(p + n, 1) && ((unsigned char)p[n] >= 0x20 || p[n] == '\n') && p[n] != 0x7f) n++;
    return n;
}

/* Research: where `needle` is reachable from the dialog object, following pointers
 * up to `depth` levels (each level scans `span` bytes of the pointed-to block). */
static size_t find_from(char *base, size_t span, int depth, const char *needle, char *path, size_t plen, char *out,
                        size_t n, size_t cap, int *hits) {
    size_t nl = strlen(needle);
    for (size_t off = 0; off + 8 <= span && n < cap && *hits < 12; off += 8) {
        if (!readable(base + off, 8)) break;
        char *p = *(char **)(base + off);
        if ((uintptr_t)p < 0x10000 || ((uintptr_t)p & 7) || !readable(p, 8)) continue;
        char sub[200];
        snprintf(sub, sizeof sub, "%s+0x%zx", path, off);
        if (readable(p, nl) && !memcmp(p, needle, nl)) {
            n += (size_t)snprintf(out + n, cap - n, "%s\"%s\"", *hits ? "," : "", sub);
            (*hits)++;
        } else if (depth > 1) {
            n = find_from(p, 0x200, depth - 1, needle, sub, plen, out, n, cap, hits);
        }
    }
    return n;
}

void nwpad_dialog_debug_find(const char *needle, char *out, size_t cap) {
    char *d = dialog_object();
    int hits = 0;
    size_t n = (size_t)snprintf(out, cap, "[");
    if (d) n = find_from(d, 0xd10, 3, needle, "", 0, out, n, cap, &hits);
    if (n < cap) snprintf(out + n, cap - n, "]");
}

void nwpad_dialog_debug_json(char *out, size_t cap) {
    char *d = dialog_object();
    if (!d) {
        snprintf(out, cap, "null");
        return;
    }
    {   /* the message's render object: class and its slot 0xd8 */
        void **obj = *(void ***)(d + 0x278);
        Dl_info a = {0}, b = {0};
        if (obj) { dladdr(*obj, &a); dladdr(((void **)*obj)[0xd8 / 8], &b); }
        size_t k = (size_t)snprintf(out, cap, "{\"render\":\"%s\",\"slot_d8\":\"%s\",\"vt\":\"%p\",\"rest\":",
                                    a.dli_sname ? a.dli_sname : "?", b.dli_sname ? b.dli_sname : "?", obj ? *obj : NULL);
        out += k; cap -= k;
    }
    int32_t count = *(int32_t *)(d + 0x130);
    size_t n = (size_t)snprintf(out, cap,
                                "{\"count\":%d,\"speaker\":%u,\"conversation\":%u,\"single\":%d,\"busy\":%d,\"replies\":[",
                                count, *(uint32_t *)(d + 0x140), *(uint32_t *)(d + 0x144), *(int32_t *)(d + 0xd08),
                                *(int32_t *)(d + 0xd04));
    exo_string *texts = *(exo_string **)(d + 0x128);
    uint32_t *ids = *(uint32_t **)(d + 0x110), *flags = *(uint32_t **)(d + 0x138);
    for (int i = 0; i < count && i < 32 && n < cap; i++) {
        char esc[600] = "";
        if (texts && texts[i].ptr) nwpad_json_escape(esc, sizeof esc, texts[i].ptr);
        n += (size_t)snprintf(out + n, cap - n, "%s{\"id\":%u,\"flags\":%u,\"text\":\"%s\"}", i ? "," : "",
                              ids ? ids[i] : 0, flags ? flags[i] : 0, esc);
    }
    if (n < cap) n += (size_t)snprintf(out + n, cap - n, "],\"strings\":[");
    /* Scan the object for pointers to text (and CExoStrings), to find the NPC's line. */
    bool first = true;
    for (size_t off = 0; off + 8 <= 0xd10 && n < cap; off += 8) {
        char *p = *(char **)(d + off);
        if ((uintptr_t)p < 0x10000 || !readable(p, 1)) continue;
        size_t len = printable_len(p, 400);
        if (len < 12) continue;
        char text[401], esc[900];
        memcpy(text, p, len);
        text[len] = '\0';
        nwpad_json_escape(esc, sizeof esc, text);
        n += (size_t)snprintf(out + n, cap - n, "%s{\"off\":\"0x%zx\",\"len\":%zu,\"text\":\"%s\"}", first ? "" : ",",
                              off, len, esc);
        first = false;
    }
    if (n < cap) snprintf(out + n, cap - n, "]}}");
}
#endif
