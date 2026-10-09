/* See nui.h. The message formats are the server's (re-notes F32):
 *   1 create:  INT token, CExoString id, INT 1, JSON definition
 *   2 destroy: INT token
 *   4 binds:   INT count, then per bind: INT token, CExoString name, JSON value
 * where JSON is a type tag (0x10), a DWORD length and that many bytes of UBJSON. */
#include "nui.h"

#include "../core/nwpad_core.h"
#include "sigs.h"
#include "vslot.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { char *ptr; uint32_t len; } exo_string; /* CExoString */

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
    exo_string copy = {(char *)s, (uint32_t)strlen(s) + 1};
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

unsigned nwpad_nui_last_size; /* debug: size of the last message */
static uint64_t game_ns;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

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
    nwpad_nui_last_size = size;
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
#define MAX_WINDOWS 4
#define MAX_BINDS 64
static struct {
    int token;
    int count;
    char names[MAX_BINDS][32];
} binds[MAX_WINDOWS];

static void record_binds(int token, const char *json) {
    int w = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (binds[i].token == token || binds[i].token == 0) { w = i; break; }
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
    for (int w = 0; w < MAX_WINDOWS; w++)
        if (binds[w].token == token) binds[w].token = 0;
    void *msg = message();
    begin(msg);
    put_int(msg, token);
    return deliver(msg, NUI_DESTROY);
}

bool nwpad_nui_events_pending(void) {
    /* std::deque: _M_start._M_cur at +0x10, _M_finish._M_cur at +0x30 */
    char *q = (char *)nwpad_sig(NWPAD_SIG_NUI_EVENT_QUEUE);
    return q && *(void **)(q + 0x10) != *(void **)(q + 0x30);
}

int nwpad_nui_window_count(void) {
    /* CGuiInGame+0x890: unordered_map<int token, shared_ptr<JsonWindow>>; size at +0x18 */
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    void *(*get_gui)(void *) = (void *(*)(void *))nwpad_sig(NWPAD_SIG_CLIENT_GET_IN_GAME_GUI);
    if (!app_manager || !*app_manager || !get_gui) return -1;
    char *gui = get_gui(*(void **)*app_manager);
    return gui ? (int)*(size_t *)(gui + 0x890 + 0x18) : -1;
}
