/* See nui.h. The message formats are the server's (re-notes F32):
 *   1 create:  INT token, CExoString id, INT 1, JSON definition
 *   2 destroy: INT token
 *   4 binds:   INT count, then per bind: INT token, CExoString name, JSON value
 * where JSON is a type tag (0x10), a DWORD length and that many bytes of UBJSON. */
#define _GNU_SOURCE /* dladdr */
#include "nui.h"

#include <dlfcn.h>

#include "../core/nwpad_core.h"
#include "game.h"
#include "sigs.h"
#include "vslot.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef nwpad_exo_string exo_string;

typedef void (*ctor_fn)(void *msg);
typedef void (*create_write_fn)(void *msg, unsigned size, unsigned player, int unknown);
typedef void (*write_int_fn)(void *msg, int value, int bits);
typedef void (*write_dword_fn)(void *msg, unsigned value, int bits);
typedef void (*write_string_fn)(void *msg, exo_string *value, int bits); /* by value: a pointer to a copy */
typedef void (*write_bytes_fn)(void *msg, const void *bytes, int count);
typedef void (*write_type_fn)(void *msg, int type);
typedef void (*get_write_fn)(void *msg, uint8_t **data, unsigned *size);
typedef int (*set_read_fn)(void *msg, uint8_t *data, unsigned size, unsigned player, int unknown);
typedef bool (*handle_fn)(void *msg, unsigned char subtype); /* true if the message read cleanly */

enum { NUI_CREATE = 1, NUI_DESTROY = 2, NUI_BINDS = 4 };

static void *message(void) {
    static _Alignas(16) uint8_t storage[0x400]; /* CNWMessage is under 0x80 bytes */
    static bool made;
    if (!made) {
        ctor_fn ctor = (ctor_fn)nwpad_sig(NWPAD_SIG_MESSAGE_CTOR);
        if (!ctor) return NULL;
        ctor(storage);
        made = true;
    }
    return storage;
}

/* A NUI window's "open" and "close" events go to the server (JsonWindow::OnOpened /
 * OnClosed queue them for FlushQueues). nwpad's windows must never reach a server,
 * so for their tokens the events are dropped; other windows are untouched. */
enum { WINDOW_TOKEN = 0x1e0 }; /* JsonWindow: int token */
static void (*window_on_opened)(void *window), (*window_on_closed)(void *window);

static bool ours(void *window) {
    int32_t token = *(int32_t *)((char *)window + WINDOW_TOKEN);
    return (token & ~0xffff) == NWPAD_NUI_TOKEN_BASE;
}

static void on_opened(void *window) {
    if (!ours(window)) window_on_opened(window);
}

static void on_closed(void *window) {
    if (!ours(window)) window_on_closed(window);
}

/* The game's NUI windows: CGuiInGame+0x890, unordered_map<int token,
 * shared_ptr<JsonWindow>> (F32); nodes are {next, int token, JsonWindow*, control}. */
static char *window_map(void) {
    char *gui = nwpad_in_game_gui();
    return gui ? gui + 0x890 : NULL;
}

/* Window `token` as the game has it now (NULL if none): looked up each time, never
 * kept, so a window the game freed is never touched. */
static void *window_of(int token) {
    char *map = window_map();
    if (!map) return NULL;
    for (char *node = *(char **)(map + 0x10); node; node = *(char **)node)
        if (*(int32_t *)(node + 8) == token) return *(void **)(node + 0x10);
    return NULL;
}

/* libstdc++ std::string and std::shared_ptr, as GetElementById takes and returns them. */
typedef struct { const char *ptr; size_t len; union { char buf[16]; size_t cap; } u; } cxx_string;
typedef struct { void *ptr; int32_t *ctrl; } cxx_shared; /* ctrl: vtable, use count (+8), weak count (+12) */
typedef void (*element_by_id_fn)(cxx_shared *ret, void *window, const cxx_string *id); /* returned in memory */

/* Window `token`'s element `id` (a Nui::Layout::Layoutable), or NULL. The window
 * keeps it alive; the reference GetElementById adds is given back at once. */
static void *element(int token, const char *id) {
    void *window = window_of(token);
    element_by_id_fn by_id = (element_by_id_fn)nwpad_sig(NWPAD_SIG_NUI_WINDOW_ELEMENT_BY_ID);
    size_t len = strlen(id);
    if (!window || !by_id || len >= 16) return NULL;
    cxx_string s = {.len = len};
    memcpy(s.u.buf, id, len + 1);
    s.ptr = s.u.buf;
    cxx_shared got = {0};
    by_id(&got, window, &s);
    if (got.ctrl) {
        /* Not the last reference (the window's tree holds the element): a plain decrement. */
        if (__atomic_load_n(&got.ctrl[2], __ATOMIC_ACQUIRE) > 1) __atomic_fetch_sub(&got.ctrl[2], 1, __ATOMIC_ACQ_REL);
        else got.ptr = NULL; /* never expected; keep the reference rather than free under the game */
    }
    return got.ptr;
}

/* An element's event callbacks (F37): seven std::function members, each _Any_data
 * (16 bytes), _M_manager, _M_invoker. All seven end in the game's event queue:
 * mousedown +0xd0 and mouseup +0xf0 (nk_vec2 pos, nk_buttons), mousescroll +0x110
 * (nk_vec2, nk_vec2), click / blur and one more at +0x130 / +0x150 / +0x170 (no
 * arguments), +0x190 (int, int). Four invoker functions serve them. */
enum { CB_DOWN = 0xd0, CB_UP = 0xf0, CB_SCROLL = 0x110, CB_VOID1 = 0x130, CB_VOID2 = 0x150, CB_VOID3 = 0x170, CB_INTS = 0x190 };
static const int cb_slots[] = {CB_DOWN, CB_UP, CB_SCROLL, CB_VOID1, CB_VOID2, CB_VOID3, CB_INTS};
#define CB_MANAGER(e, off) (*(void **)((char *)(e) + (off) + 0x10))
#define CB_INVOKER(e, off) (*(void **)((char *)(e) + (off) + 0x18))

#define MAX_TAKEN 24
static struct { char *element; int token, tag; } taken[MAX_TAKEN];
static nwpad_nui_input inputs[32];
static unsigned inputs_head, inputs_tail;

static void queue_input(const void *functor, int slot, int kind, int button, float x, float y) {
    char *e = (char *)functor - slot;
    for (int i = 0; i < MAX_TAKEN; i++)
        if (taken[i].element == e && taken[i].token) {
            if (inputs_tail - inputs_head < sizeof inputs / sizeof inputs[0])
                inputs[inputs_tail++ % (sizeof inputs / sizeof inputs[0])] =
                    (nwpad_nui_input){taken[i].token, taken[i].tag, kind, button, x, y};
            return;
        }
}

/* nwpad's invokers (libstdc++: the functor's storage, then each argument by reference).
 * They run inside the game's NUI input pass, so they only record. */
static void on_down(const void *f, const float *pos, const int *button) { queue_input(f, CB_DOWN, NWPAD_NUI_DOWN, *button, pos[0], pos[1]); }
static void on_up(const void *f, const float *pos, const int *button) { queue_input(f, CB_UP, NWPAD_NUI_UP, *button, pos[0], pos[1]); }
static void on_scroll(const void *f, const float *a, const float *b) {
    (void)a;
    queue_input(f, CB_SCROLL, NWPAD_NUI_SCROLL, 0, b[0], b[1]);
}
/* The rest: nothing to do, and nothing queued. */
static void on_nothing(const void *f) { (void)f; }
static void on_nothing_ints(const void *f, const int *a, const int *b) { (void)f, (void)a, (void)b; }

static void forget_taken(int token) {
    for (int i = 0; i < MAX_TAKEN; i++)
        if (taken[i].token == token) taken[i].token = 0, taken[i].element = NULL;
}

/* The game's four invokers, from the first element checked (all elements share them). */
static void *game_invokers[4];

static bool callbacks_as_expected(char *e) {
    void *inv[7];
    for (int i = 0; i < 7; i++) {
        if (!CB_MANAGER(e, cb_slots[i]) || !(inv[i] = CB_INVOKER(e, cb_slots[i]))) return false;
    }
    if (inv[0] != inv[1] || inv[3] != inv[4] || inv[3] != inv[5]) return false;
    void *four[4] = {inv[0], inv[2], inv[3], inv[6]};
    if (!game_invokers[0]) {
        Dl_info game, info;
        if (!dladdr(nwpad_sig(NWPAD_SIG_NUI_WINDOW_ON_OPENED), &game)) return false;
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < i; j++)
                if (four[i] == four[j]) return false;
            if (!dladdr(four[i], &info) || info.dli_fbase != game.dli_fbase) return false; /* the game's code */
        }
        memcpy(game_invokers, four, sizeof four);
    }
    return !memcmp(game_invokers, four, sizeof four);
}

bool nwpad_nui_take_input(int token, const char *id, int tag) {
    char *e = element(token, id);
    if (!e) return false;
    int free_slot = -1;
    for (int i = 0; i < MAX_TAKEN; i++) {
        if (taken[i].token && taken[i].element == e) {
            if (CB_INVOKER(e, CB_DOWN) == (void *)on_down) return true; /* already ours */
            taken[i].token = 0; /* a freed window's element, its memory reused: take the new one */
        }
        if (!taken[i].token && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0 || !callbacks_as_expected(e)) return false;
    taken[free_slot].element = e, taken[free_slot].token = token, taken[free_slot].tag = tag;
    CB_INVOKER(e, CB_DOWN) = (void *)on_down;
    CB_INVOKER(e, CB_UP) = (void *)on_up;
    CB_INVOKER(e, CB_SCROLL) = (void *)on_scroll;
    CB_INVOKER(e, CB_VOID1) = CB_INVOKER(e, CB_VOID2) = CB_INVOKER(e, CB_VOID3) = (void *)on_nothing;
    CB_INVOKER(e, CB_INTS) = (void *)on_nothing_ints;
    return true;
}

bool nwpad_nui_next_input(nwpad_nui_input *out) {
    if (inputs_head == inputs_tail) return false;
    *out = inputs[inputs_head++ % (sizeof inputs / sizeof inputs[0])];
    return true;
}


static bool hook_events(void) {
    static int state; /* 0 untried, 1 hooked, -1 failed */
    if (state) return state > 0;
    void *vtable = nwpad_sig(NWPAD_SIG_NUI_WINDOW_VTABLE), *orig;
    bool ok = vtable && nwpad_vslot_swap(vtable, 16, nwpad_sig(NWPAD_SIG_NUI_WINDOW_ON_OPENED), (void *)on_opened, &orig);
    if (ok) window_on_opened = (void (*)(void *))orig;
    ok = ok && nwpad_vslot_swap(vtable, 16, nwpad_sig(NWPAD_SIG_NUI_WINDOW_ON_CLOSED), (void *)on_closed, &orig);
    if (ok) window_on_closed = (void (*)(void *))orig;
    state = ok ? 1 : -1;
    return ok;
}

static bool ready(void) {
    static const int ids[] = {NWPAD_SIG_NUI_HANDLE_SERVER_MESSAGE, NWPAD_SIG_MESSAGE_CTOR,
                              NWPAD_SIG_MESSAGE_CREATE_WRITE,       NWPAD_SIG_MESSAGE_WRITE_INT,
                              NWPAD_SIG_MESSAGE_WRITE_DWORD,        NWPAD_SIG_MESSAGE_WRITE_STRING,
                              NWPAD_SIG_MESSAGE_WRITE_BYTES,        NWPAD_SIG_MESSAGE_GET_WRITE,
                              NWPAD_SIG_MESSAGE_SET_READ,           NWPAD_SIG_CLIENT_GET_IN_GAME_GUI,
                              NWPAD_SIG_MESSAGE_WRITE_TYPE,         NWPAD_SIG_NUI_WINDOW_VTABLE,
                              NWPAD_SIG_NUI_WINDOW_ON_OPENED,       NWPAD_SIG_NUI_WINDOW_ON_CLOSED};
    /* No window without the event filter: nothing of ours may reach a server. */
    return nwpad_sigs_all(ids, sizeof ids / sizeof ids[0]) && hook_events() && message();
}

static void begin(void *msg) { ((create_write_fn)nwpad_sig(NWPAD_SIG_MESSAGE_CREATE_WRITE))(msg, 0x400, 0xffffffffu, 1); }
static void put_int(void *msg, int v) { ((write_int_fn)nwpad_sig(NWPAD_SIG_MESSAGE_WRITE_INT))(msg, v, 32); }

static void put_string(void *msg, const char *s) {
    exo_string copy = {(char *)s, (uint32_t)strlen(s) + 1, 0};
    ((write_string_fn)nwpad_sig(NWPAD_SIG_MESSAGE_WRITE_STRING))(msg, &copy, 32);
}

static bool put_json(void *msg, const char *json) {
    static uint8_t ubj[32768];
    int n = nwpad_ubjson_from_json(json, ubj, sizeof ubj);
    if (n < 0) return false;
    ((write_type_fn)nwpad_sig(NWPAD_SIG_MESSAGE_WRITE_TYPE))(msg, 0x10); /* as CNWMessage::WriteJSON does */
    ((write_dword_fn)nwpad_sig(NWPAD_SIG_MESSAGE_WRITE_DWORD))(msg, (unsigned)n, 32);
    ((write_bytes_fn)nwpad_sig(NWPAD_SIG_MESSAGE_WRITE_BYTES))(msg, ubj, n);
    return true;
}

#ifdef NWPAD_DEBUG_SURFACES
unsigned nwpad_nui_last_size; /* the last message's size */
#endif
static uint64_t game_ns;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

void nwpad_game_ns_add(uint64_t ns) { game_ns += ns; }
uint64_t nwpad_now_ns(void) { return now_ns(); }

uint64_t nwpad_nui_take_game_ns(void) {
    uint64_t ns = game_ns;
    game_ns = 0;
    return ns;
}

/* Hand the finished message to the game's handler, as if the server sent it. */
static bool deliver(void *msg, unsigned char subtype) {
    uint8_t *data = NULL;
    unsigned size = 0;
    ((get_write_fn)nwpad_sig(NWPAD_SIG_MESSAGE_GET_WRITE))(msg, &data, &size);
#ifdef NWPAD_DEBUG_SURFACES
    nwpad_nui_last_size = size;
#endif
    /* The write buffer starts with room for the 3-byte packet header (type, major,
     * minor), which the network layer skips before SetReadMessage (F32). */
    if (!data || size < 3 ||
        !((set_read_fn)nwpad_sig(NWPAD_SIG_MESSAGE_SET_READ))(msg, data + 3, size - 3, 0xffffffffu, 1))
        return false;
    uint64_t t0 = now_ns();
    bool ok = ((handle_fn)nwpad_sig(NWPAD_SIG_NUI_HANDLE_SERVER_MESSAGE))(msg, subtype);
    game_ns += now_ns() - t0;
    return ok;
}

/* Setting a bind no element of the window uses makes the client send it to the
 * server (DynamicBinding::NotifyParent, F32). So each window's bind names are
 * taken from its definition, and only those may be set. */
#define MAX_WINDOWS 8
#define MAX_BINDS 64
static struct {
    int token;
    int count;
    char names[MAX_BINDS][32];
} binds[MAX_WINDOWS];

static void record_binds(int token, const char *json) {
    int w = -1;
    for (int i = 0; i < MAX_WINDOWS && w < 0; i++) /* the window's own entry, else a free one */
        if (binds[i].token == token) w = i;
    for (int i = 0; i < MAX_WINDOWS && w < 0; i++)
        if (binds[i].token == 0) w = i;
    if (w < 0) w = 0; /* more windows than entries: never expected */
    binds[w].token = token;
    binds[w].count = 0;
    for (const char *p = json; (p = strstr(p, "\"bind\"")) != NULL; p += 6) {
        const char *q = p + 6;
        while (*q == ' ' || *q == ':') q++;
        if (*q != '"') continue;
        const char *end = strchr(++q, '"');
        if (!end || end - q >= 32) continue;
        bool seen = false;
        for (int i = 0; i < binds[w].count; i++)
            if ((size_t)(end - q) == strlen(binds[w].names[i]) && !strncmp(binds[w].names[i], q, (size_t)(end - q))) seen = true;
        if (!seen && binds[w].count < MAX_BINDS) {
            memcpy(binds[w].names[binds[w].count], q, (size_t)(end - q));
            binds[w].names[binds[w].count++][end - q] = '\0';
        }
    }
}

static bool bind_known(int token, const char *name) {
    for (int w = 0; w < MAX_WINDOWS; w++)
        if (binds[w].token == token)
            for (int i = 0; i < binds[w].count; i++)
                if (!strcmp(binds[w].names[i], name)) return true;
    return false;
}

bool nwpad_nui_create(int token, const char *id, const char *json) {
    if (!ready()) return false;
    record_binds(token, json);
    void *msg = message();
    begin(msg);
    put_int(msg, token);
    put_string(msg, id);
    put_int(msg, 1); /* definition inline (0 would load it from a resource) */
    if (!put_json(msg, json)) return false;
    return deliver(msg, NUI_CREATE);
}

bool nwpad_nui_bind(int token, const char *name, const char *json_value) {
    if (!ready() || !bind_known(token, name)) return false;
    void *msg = message();
    begin(msg);
    put_int(msg, 1);
    put_int(msg, token);
    put_string(msg, name);
    if (!put_json(msg, json_value)) return false;
    return deliver(msg, NUI_BINDS);
}

bool nwpad_nui_destroy(int token) {
    if (!ready()) return false;
    forget_taken(token);
    for (int w = 0; w < MAX_WINDOWS; w++)
        if (binds[w].token == token) binds[w].token = 0;
    void *msg = message();
    begin(msg);
    put_int(msg, token);
    return deliver(msg, NUI_DESTROY);
}

#ifdef NWPAD_DEBUG_SURFACES
bool nwpad_nui_events_pending(void) {
    /* std::deque: _M_start._M_cur at +0x10, _M_finish._M_cur at +0x30 */
    char *q = (char *)nwpad_sig(NWPAD_SIG_NUI_EVENT_QUEUE);
    return q && *(void **)(q + 0x10) != *(void **)(q + 0x30);
}

int nwpad_nui_window_count(void) {
    char *map = window_map(); /* size at +0x18 */
    return map ? (int)*(size_t *)(map + 0x18) : -1;
}
#endif
