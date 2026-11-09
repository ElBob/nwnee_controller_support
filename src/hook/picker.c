/* See picker.h. One window holds three wheels, one per quickbar bank, side by side
 * (Shift, Plain, Ctrl): the active bank's at full size, the other two at SMALL (no text). Two
 * small wheels and a big one are the same total width in any order, so the row keeps
 * its width, stays centred, and changing the active bank just scales the wheels in
 * place over ANIM_NS (the neighbours slide to make room). Every wheel's rects are
 * binds, so nothing is rebuilt; text can't scale, so it's hidden while they move. */
#include "picker.h"

#include "nui.h"
#include "quickbar.h"
#include "dialog.h"
#include "game.h"
#include "sigs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TOKEN (NWPAD_NUI_TOKEN_BASE + 0x100)
#define SIZE 440.0f     /* a full-size wheel, GUI units */
#define RING 160.0f     /* ring radius */
#define ICON 48.0f
#define ICON_PICKED 68.0f
#define NAME_H 36.0f
#define PAD 28.0f       /* room for NUI's own padding (else it adds scrollbars) */
#define SMALL 0.5f      /* the other wheels' scale */
#define GAP 24.0f       /* between wheels */
#define BANKS 3
/* The wheels, left to right, are Shift, Plain, Ctrl (banks 1, 0, 2); the picker always
 * opens on the middle one (Plain), whatever the quickbar shows. Positions are
 * 0 (left), 1, 2; everything on screen, and the binds' names, go by position. */
static const int ORDER[BANKS] = {1, 0, 2};
#define START_POS 1
#define WIDTH (SIZE * (1 + 2 * SMALL) + 2 * GAP) /* the row: one big wheel, two small, any order */
#define ANIM_NS 180000000ull
#define BATCH 96        /* binds per message: 3 discs, 36 slots, 36 names, and flags */

static struct {
    bool open, animating;
    int pos, bank, selected, last_used; /* the big wheel's place, its bank; last_used: quickbar slot 0-35, -1 none */
    float scale[BANKS], from[BANKS]; /* each wheel's scale now, and when the slide began */
    float sent_scale[BANKS], sent_x[BANKS]; /* what each wheel's binds were last set to (-1: nothing yet) */
    uint64_t start_ns;
    nwpad_qb_slot slots[NWPAD_QB_SLOTS];
} pk = {.selected = -1, .last_used = -1};

int nwpad_picker_selected(void) { return pk.open ? pk.selected : -1; }
bool nwpad_picker_open(void) { return pk.open; }
int nwpad_picker_last_used(void) { return pk.last_used; }
int nwpad_picker_bank(void) { return pk.open ? pk.bank : -1; }
bool nwpad_picker_animating(void) { return pk.open && pk.animating; }

static const nwpad_qb_slot *bank_slot(int bank, int i) { return &pk.slots[bank * NWPAD_PICKER_SLOTS + i]; }
static bool has_icon(const nwpad_qb_slot *s) { return s->type != 0 && (s->icon[0] || s->parts[0][0]); }
static bool name_only(const nwpad_qb_slot *s) { return s->type != 0 && !has_icon(s); } /* shown as its name */
static int bank_at(int pos) { return ORDER[pos]; }

/* Whether wheel w has a slot shown by its name (only then does the window define the
 * wheel's name / dot visibility binds; a bind the window doesn't use isn't sent). */
static bool has_names(int w) {
    for (int i = 0; i < NWPAD_PICKER_SLOTS; i++)
        if (name_only(bank_slot(bank_at(w), i))) return true;
    return false;
}

static void rest_scale(int pos, float s[BANKS]) {
    for (int w = 0; w < BANKS; w++) s[w] = w == pos ? 1.0f : SMALL;
}

/* Wheel centres along the row (window content coordinates) for the given scales. */
static void geometry(const float s[BANKS], float cx[BANKS]) {
    float x = 0;
    for (int w = 0; w < BANKS; w++) {
        cx[w] = x + SIZE * s[w] / 2;
        x += SIZE * s[w] + GAP;
    }
}

static void slot_centre_at(int i, float wx, float scale, float *x, float *y) {
    float a = ((float)i * 360.0f / NWPAD_PICKER_SLOTS - 90.0f) * NWPAD_RAD_PER_DEG;
    *x = wx + RING * scale * cosf(a);
    *y = SIZE / 2 + RING * scale * sinf(a);
}

static int rect_json(char *out, size_t cap, float cx, float cy, float size) {
    return snprintf(out, cap, "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", cx - size / 2, cy - size / 2, size, size);
}

/* The screen's centre in GUI units. */
static void screen_centre(float *x, float *y) {
    nwpad_gui_size(x, y);
    *x /= 2;
    *y /= 2;
}

/* ---- Binds, sent in batches ---- */

static struct { char name[16]; char value[96]; } batch[BATCH];
static int batch_n;

static void put(const char *name, const char *value) {
    if (batch_n >= BATCH) return;
    snprintf(batch[batch_n].name, sizeof batch[0].name, "%s", name);
    snprintf(batch[batch_n].value, sizeof batch[0].value, "%s", value);
    batch_n++;
}

static void put_rect(const char *name, float cx, float cy, float size) {
    char rect[96];
    rect_json(rect, sizeof rect, cx, cy, size);
    put(name, rect);
}

static void flush(void) {
    const char *names[BATCH], *values[BATCH];
    for (int i = 0; i < batch_n; i++) {
        names[i] = batch[i].name;
        values[i] = batch[i].value;
    }
    nwpad_nui_bind_many(TOKEN, batch_n, names, values);
    batch_n = 0;
}

/* The disc and slot rects of every wheel that is not where its binds already have it
 * (a slide from the middle moves two of the three) for these scales; and, if `text`,
 * the names' rects. */
static void apply_layout(const float s[BANKS], bool text) {
    float cx[BANKS], x, y;
    char name[16];
    geometry(s, cx);
    for (int w = 0; w < BANKS; w++) {
        if (!text && fabsf(s[w] - pk.sent_scale[w]) < 1e-3f && fabsf(cx[w] - pk.sent_x[w]) < 0.05f) continue;
        pk.sent_scale[w] = s[w];
        pk.sent_x[w] = cx[w];
        snprintf(name, sizeof name, "d%d", w);
        put_rect(name, cx[w], SIZE / 2, (2 * RING + 100) * s[w]);
        for (int i = 0; i < NWPAD_PICKER_SLOTS; i++) {
            slot_centre_at(i, cx[w], s[w], &x, &y);
            snprintf(name, sizeof name, "s%d_%d", w, i);
            put_rect(name, x, y, ICON * s[w]);
            if (text && name_only(bank_slot(bank_at(w), i))) {
                char rect[96];
                snprintf(name, sizeof name, "x%d_%d", w, i);
                snprintf(rect, sizeof rect, "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":20.0}", x - ICON / 2 - 6, y - 10, ICON + 12);
                put(name, rect);
            }
        }
    }
    flush();
}

/* Names shown on the active wheel (-1: on none, while the wheels move); the faint dot
 * a name-only slot has otherwise goes the other way. */
static void set_names(int active) {
    char name[16];
    for (int w = 0; w < BANKS; w++) {
        if (!has_names(w)) continue;
        snprintf(name, sizeof name, "e%d", w);
        put(name, w == active ? "true" : "false");
        snprintf(name, sizeof name, "o%d", w);
        put(name, w == active ? "false" : "true");
    }
    flush();
}

/* ---- The window ---- */

/* One wheel's draw-list items: disc, then each slot (an icon's images, or a faint
 * dot, and for a slot with only a name that name). All positions are binds. */
static size_t draw_wheel(char *json, size_t n, size_t cap, int w) {
    char esc[300];
#define EMIT(...) do { if (n < cap) n += (size_t)snprintf(json + n, cap - n, __VA_ARGS__); } while (0)
    EMIT("%s{\"type\":2,\"enabled\":true,\"color\":{\"r\":0,\"g\":0,\"b\":0,\"a\":150},\"fill\":true,"
         "\"line_thickness\":1.0,\"order\":-1,\"render\":0,\"arrayBinds\":false,\"rect\":{\"bind\":\"d%d\"}}",
         w ? "," : "", w);
    for (int i = 0; i < NWPAD_PICKER_SLOTS; i++) {
        const nwpad_qb_slot *s = bank_slot(bank_at(w), i);
        if (has_icon(s)) { /* one image, or an item's parts bottom to top (F34) */
            const char *refs[3] = {s->icon[0] ? s->icon : s->parts[0], s->icon[0] ? "" : s->parts[1],
                                   s->icon[0] ? "" : s->parts[2]};
            for (int k = 0; k < 3; k++) {
                if (!refs[k][0]) continue;
                nwpad_json_escape(esc, sizeof esc, refs[k]);
                EMIT(",{\"type\":5,\"enabled\":true,\"color\":null,\"fill\":null,\"line_thickness\":null,\"order\":1,"
                     "\"render\":0,\"arrayBinds\":false,\"image\":\"%s\",\"rect\":{\"bind\":\"s%d_%d\"},"
                     "\"image_aspect\":0,\"image_halign\":0,\"image_valign\":0}", esc, w, i);
            }
            continue;
        }
        /* empty, or a name-only slot while its wheel shows no names: a faint dot */
        if (name_only(s)) EMIT(",{\"type\":2,\"enabled\":{\"bind\":\"o%d\"},", w);
        else EMIT(",{\"type\":2,\"enabled\":true,");
        EMIT("\"color\":{\"r\":255,\"g\":255,\"b\":255,\"a\":40},\"fill\":false,\"line_thickness\":1.0,\"order\":1,"
             "\"render\":0,\"arrayBinds\":false,\"rect\":{\"bind\":\"s%d_%d\"}}", w, i);
        if (name_only(s)) {
            char shortname[12];
            snprintf(shortname, sizeof shortname, "%s", s->name);
            nwpad_game_json(esc, sizeof esc, shortname);
            EMIT(",{\"type\":4,\"enabled\":{\"bind\":\"e%d\"},\"color\":{\"r\":230,\"g\":220,\"b\":190,\"a\":255},"
                 "\"fill\":null,\"line_thickness\":null,\"order\":1,\"render\":0,\"arrayBinds\":false,"
                 "\"rect\":{\"bind\":\"x%d_%d\"},\"text\":\"%s\"}", w, w, i, esc);
        }
    }
#undef EMIT
    return n;
}

static bool build(void) {
    static char json[49152];
    float cx, cy;
    screen_centre(&cx, &cy);
    size_t n = (size_t)snprintf(json, sizeof json,
        "{\"version\":1,\"title\":false,\"resizable\":false,\"collapsed\":false,\"closable\":false,"
        "\"transparent\":true,\"border\":false,\"accepts_input\":false,\"size_constraint\":null,"
        "\"edge_constraint\":null,\"font\":\"\",\"geometry\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f},"
        /* The wheels are the root column's draw list; its children put the name in
         * the middle of the row: spacer, label, spacer. */
        "\"root\":{\"type\":\"col\",\"label\":null,\"value\":null,\"draw_list_scissor\":false,\"children\":["
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f},"
        "{\"type\":\"label\",\"label\":null,\"value\":{\"bind\":\"name\"},\"text_halign\":0,\"text_valign\":0,"
        "\"height\":%.1f},"
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f}],"
        "\"draw_list\":[",
        cx - (WIDTH + PAD) / 2, cy - (SIZE + PAD) / 2, WIDTH + PAD, SIZE + PAD, (SIZE - NAME_H) / 2, NAME_H,
        (SIZE - NAME_H) / 2);
    for (int w = 0; w < BANKS; w++) n = draw_wheel(json, n, sizeof json, w);
    if (n < sizeof json)
        n += (size_t)snprintf(json + n, sizeof json - n,
            ",{\"type\":2,\"enabled\":{\"bind\":\"hl_on\"},\"color\":{\"r\":235,\"g\":190,\"b\":80,\"a\":255},\"fill\":false,"
            "\"line_thickness\":3.0,\"order\":1,\"render\":0,\"arrayBinds\":false,\"rect\":{\"bind\":\"hl\"}}]}}");
    if (n >= sizeof json || !nwpad_nui_create(TOKEN, "nwpad_picker", json)) return false;
    rest_scale(pk.pos, pk.scale);
    pk.animating = false;
    for (int w = 0; w < BANKS; w++) pk.sent_scale[w] = pk.sent_x[w] = -1;
    apply_layout(pk.scale, true);
    set_names(pk.pos);
    nwpad_nui_bind(TOKEN, "hl_on", "false");
    nwpad_nui_bind(TOKEN, "hl", "{\"x\":0.0,\"y\":0.0,\"w\":1.0,\"h\":1.0}");
    nwpad_nui_bind(TOKEN, "name", "\"\"");
    return true;
}

/* Highlight slot `now` of the active wheel (at rest), and show its name; `previous`
 * (-1 none) goes back to normal size. */
static void show_selection(int previous, int now) {
    char rect[96], bind[16], esc[300], value[320];
    float cx[BANKS], x, y, s[BANKS];
    rest_scale(pk.pos, s);
    geometry(s, cx);
    int w = pk.pos;
    if (previous >= 0 && has_icon(bank_slot(pk.bank, previous))) {
        slot_centre_at(previous, cx[w], 1.0f, &x, &y);
        rect_json(rect, sizeof rect, x, y, ICON);
        snprintf(bind, sizeof bind, "s%d_%d", w, previous);
        nwpad_nui_bind(TOKEN, bind, rect);
    }
    slot_centre_at(now, cx[w], 1.0f, &x, &y);
    if (has_icon(bank_slot(pk.bank, now))) {
        rect_json(rect, sizeof rect, x, y, ICON_PICKED);
        snprintf(bind, sizeof bind, "s%d_%d", w, now);
        nwpad_nui_bind(TOKEN, bind, rect);
    }
    rect_json(rect, sizeof rect, x, y, ICON_PICKED + 10);
    nwpad_nui_bind(TOKEN, "hl", rect);
    nwpad_nui_bind(TOKEN, "hl_on", "true");
    const nwpad_qb_slot *slot = bank_slot(pk.bank, now);
    nwpad_game_json(esc, sizeof esc, slot->type ? slot->name : "");
    snprintf(value, sizeof value, "\"%s\"", esc);
    nwpad_nui_bind(TOKEN, "name", value);
}

/* ---- The slide ---- */

static float ease(float t) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

static void animate(void) {
    float rest[BANKS], s[BANKS];
    float t = (float)(nwpad_now_ns() - pk.start_ns) / (float)ANIM_NS;
    rest_scale(pk.pos, rest);
    for (int w = 0; w < BANKS; w++) s[w] = pk.from[w] + (rest[w] - pk.from[w]) * ease(t);
    memcpy(pk.scale, s, sizeof s);
    if (t < 1) {
        apply_layout(s, false);
        return;
    }
    pk.animating = false;
    apply_layout(rest, true); /* settled: the names, the highlight */
    set_names(pk.pos);
    if (pk.selected >= 0) show_selection(-1, pk.selected);
}

/* The wheels keep their places: `[` and `]` make the neighbour on that side the big
 * one, and at either end do nothing (the key is still the picker's). */
bool nwpad_picker_shift(int direction) {
    if (!pk.open) return false;
    int next = pk.pos + (direction < 0 ? -1 : 1);
    if (next < 0 || next >= BANKS) return true;
    memcpy(pk.from, pk.scale, sizeof pk.from); /* from wherever the wheels are (a slide in progress) */
    pk.pos = next;
    pk.bank = bank_at(next);
    pk.animating = true;
    pk.start_ns = nwpad_now_ns();
    nwpad_nui_bind(TOKEN, "hl_on", "false");
    nwpad_nui_bind(TOKEN, "name", "\"\"");
    set_names(-1);
    return true;
}

void nwpad_picker_close(bool use) {
    if (!pk.open) return;
    nwpad_nui_destroy(TOKEN);
    pk.open = pk.animating = false;
    if (use && pk.selected >= 0 && bank_slot(pk.bank, pk.selected)->type != 0 &&
        nwpad_quickbar_use(pk.bank * NWPAD_PICKER_SLOTS + pk.selected))
        pk.last_used = pk.bank * NWPAD_PICKER_SLOTS + pk.selected;
    pk.selected = -1;
}

bool nwpad_picker_frame(bool want, nwpad_vec2 right, bool in_game) {
    if (!pk.open) {
        if (!want || !in_game) return false;
        if (!nwpad_quickbar_read(pk.slots)) return false; /* all three banks, shown or not */
        pk.pos = START_POS;
        pk.bank = bank_at(START_POS);
        pk.selected = -1;
        pk.open = build();
        return pk.open;
    }
    if (want && in_game) {
        if (pk.animating) animate();
        int now = nwpad_picker_select(right, pk.selected);
        if (now != pk.selected && now >= 0 && !pk.animating) show_selection(pk.selected, now);
        pk.selected = now; /* (while sliding, the end of the slide shows it) */
        return true;
    }
    /* No longer wanted (cancelled, or out of the game): close without using. */
    nwpad_picker_close(false);
    return false;
}
