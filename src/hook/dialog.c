/* See dialog.h. Layout and paths: re-notes F36. */
#include "dialog.h"

#include "../core/nwpad_core.h"
#include "sigs.h"
#include "vslot.h"

#include <stdio.h>
#include <string.h>

typedef struct { char *ptr; uint32_t len, cap; } exo_string; /* CExoString, 16 bytes */
typedef void *(*vcall_fn)(void *self);
typedef void (*set_text_fn)(void *self, const char *text);
typedef void (*gui_int_fn)(void *gui, int n);

enum {
    GUI_DIALOG = 0x70,         /* CGuiInGame -> CGuiInGameChatDialog* */
    DLG_MESSAGE_TEXT = 0x278,  /* the NPC line's StringGob (secondary base, as SetText's thunk sees it) */
    DLG_REPLY_IDS = 0x110,     /* unsigned[] */
    DLG_REPLY_TEXTS = 0x128,   /* CExoString[] */
    DLG_REPLY_COUNT = 0x130,   /* int */
    DLG_REPLY_FLAGS = 0x138,   /* unsigned[]; bit 0: not selectable */
    DLG_SPEAKER = 0x140,       /* object id */
    DLG_BUSY = 0xd04,          /* int */
    DLG_HEIGHT = 0xd00,        /* float, scene units (~100 GUI pixels each; FixDialogSize) */
};

static struct {
    set_text_fn set_text;      /* the original StringGob::SetText thunk */
    char line[4096];
    void *line_owner;          /* the StringGob the line was set on */
    uint32_t seq;
    void *replies_seen;        /* reply array last read: a new one means new replies */
} dl;

static void *gui(void) {
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    vcall_fn get_gui = (vcall_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_IN_GAME_GUI);
    return app_manager && *app_manager && get_gui ? get_gui(*(void **)*app_manager) : NULL;
}

static char *dialog_object(void) {
    char *g = gui();
    return g ? *(char **)(g + GUI_DIALOG) : NULL;
}

/* Every GUI text change passes here; keep the open dialog's NPC line. */
static void on_set_text(void *self, const char *text) {
    dl.set_text(self, text);
    char *d = dialog_object();
    if (d && self == *(void **)(d + DLG_MESSAGE_TEXT)) {
        snprintf(dl.line, sizeof dl.line, "%s", text ? text : "");
        dl.line_owner = self;
        dl.seq++;
    }
}

bool nwpad_dialog_init(void) {
    static int state; /* 0 untried, 1 ok, -1 unavailable */
    if (state) return state > 0;
    static const int ids[] = {NWPAD_SIG_STRING_GOB_VTABLE, NWPAD_SIG_STRING_GOB_SET_TEXT_THUNK,
                              NWPAD_SIG_GUI_DIALOG_NUM_KEY, NWPAD_SIG_GUI_DIALOG_SELECTION,
                              NWPAD_SIG_CLIENT_GET_IN_GAME_GUI, NWPAD_SIG_APP_MANAGER};
    void *orig = NULL;
    /* The thunk sits in one of StringGob's secondary vtables, inside its vtable group. */
    bool ok = nwpad_sigs_all(ids, sizeof ids / sizeof ids[0]) &&
              nwpad_vslot_swap(nwpad_sig(NWPAD_SIG_STRING_GOB_VTABLE), 512, nwpad_sig(NWPAD_SIG_STRING_GOB_SET_TEXT_THUNK),
                               (void *)on_set_text, &orig);
    if (ok) dl.set_text = (set_text_fn)orig;
    state = ok ? 1 : -1;
    return ok;
}

uint32_t nwpad_dialog_seq(void) {
    char *d = dialog_object();
    if (!d) {
        dl.line_owner = NULL;
        return 0;
    }
    if (*(void **)(d + DLG_REPLY_TEXTS) != dl.replies_seen) { /* SetReplies reallocates */
        dl.replies_seen = *(void **)(d + DLG_REPLY_TEXTS);
        dl.seq++;
    }
    return dl.seq ? dl.seq : (dl.seq = 1);
}

/* The speaker's name and portrait, as ShowDialogEntry finds them (F36). */
static void speaker_info(uint32_t oid, nwpad_dialog *out) {
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    void *(*object)(void *, uint32_t) = (void *(*)(void *, uint32_t))nwpad_sig(NWPAD_SIG_CLIENT_GET_GAME_OBJECT);
    int (*name_of)(void *, uint32_t, exo_string *) =
        (int (*)(void *, uint32_t, exo_string *))nwpad_sig(NWPAD_SIG_CLIENT_GET_OBJECT_NAME);
    void (*dtor)(exo_string *) = (void (*)(exo_string *))nwpad_sig(NWPAD_SIG_EXO_STRING_DTOR);
    if (!app_manager || !*app_manager) return;
    void *app = *(void **)*app_manager;
    if (name_of && dtor) {
        exo_string s = {0};
        if (name_of(app, oid, &s) && s.ptr) snprintf(out->speaker_name, sizeof out->speaker_name, "%s", s.ptr);
        dtor(&s);
    }
    void **obj = object ? object(app, oid) : NULL;
    void **nwc = obj ? ((void **(*)(void *))((void **)*obj)[0x20 / 8])(obj) : NULL; /* AsNWCObject */
    if (nwc) {
        char ref[32] = {0}; /* CResRef */
        ((void (*)(char *, void *, int))((void **)*nwc)[0x1d8 / 8])(ref, nwc, 3); /* GetPortrait(size) */
        snprintf(out->portrait, sizeof out->portrait, "%.16s", ref);
    }
}

bool nwpad_dialog_read(nwpad_dialog *out) {
    memset(out, 0, sizeof *out);
    char *d = dialog_object();
    if (!d) {
        dl.line_owner = NULL;
        return false;
    }
    out->open = true;
    out->seq = nwpad_dialog_seq();
    out->speaker = *(uint32_t *)(d + DLG_SPEAKER);
    out->panel_h = *(float *)(d + DLG_HEIGHT) * 100.0f;
    speaker_info(out->speaker, out);
    out->busy = *(int32_t *)(d + DLG_BUSY) != 0;
    if (dl.line_owner == *(void **)(d + DLG_MESSAGE_TEXT)) snprintf(out->line, sizeof out->line, "%s", dl.line);
    int count = *(int32_t *)(d + DLG_REPLY_COUNT);
    exo_string *texts = *(exo_string **)(d + DLG_REPLY_TEXTS);
    uint32_t *rids = *(uint32_t **)(d + DLG_REPLY_IDS), *flags = *(uint32_t **)(d + DLG_REPLY_FLAGS);
    if (count < 0 || !texts || !rids) count = 0;
    if (count > NWPAD_DIALOG_REPLIES) count = NWPAD_DIALOG_REPLIES;
    out->count = count;
    for (int i = 0; i < count; i++) {
        out->replies[i].id = rids[i];
        out->replies[i].selectable = !flags || !(flags[i] & 1);
        snprintf(out->replies[i].text, sizeof out->replies[i].text, "%s", texts[i].ptr ? texts[i].ptr : "");
    }
    return true;
}

bool nwpad_dialog_select(int index) {
    void *g = gui();
    char *d = dialog_object();
    if (!g || !d || index < 0 || index >= *(int32_t *)(d + DLG_REPLY_COUNT)) return false;
    ((gui_int_fn)nwpad_sig(NWPAD_SIG_GUI_DIALOG_NUM_KEY))(g, index + 1);
    return true;
}

bool nwpad_dialog_end(void) {
    void *g = gui();
    if (!g || !dialog_object()) return false;
    ((gui_int_fn)nwpad_sig(NWPAD_SIG_GUI_DIALOG_SELECTION))(g, -3);
    return true;
}

#ifdef NWPAD_DEBUG_SURFACES
void nwpad_dialog_debug_json(char *out, size_t cap) {
    static nwpad_dialog d;
    if (!nwpad_dialog_read(&d)) {
        snprintf(out, cap, "null");
        return;
    }
    static char esc[9000];
    nwpad_json_escape(esc, sizeof esc, d.line);
    char name[300];
    nwpad_json_escape(name, sizeof name, d.speaker_name);
    size_t n = (size_t)snprintf(out, cap,
                                "{\"seq\":%u,\"speaker\":%u,\"name\":\"%s\",\"portrait\":\"%s\",\"panel_h\":%.0f,"
                                "\"busy\":%s,\"line\":\"%s\",\"replies\":[",
                                d.seq, d.speaker, name, d.portrait, d.panel_h, d.busy ? "true" : "false", esc);
    for (int i = 0; i < d.count && n < cap; i++) {
        nwpad_json_escape(esc, sizeof esc, d.replies[i].text);
        n += (size_t)snprintf(out + n, cap - n, "%s{\"id\":%u,\"selectable\":%s,\"text\":\"%s\"}", i ? "," : "",
                              d.replies[i].id, d.replies[i].selectable ? "true" : "false", esc);
    }
    if (n < cap) snprintf(out + n, cap - n, "]}");
}
#endif
