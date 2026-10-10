/* See picker.h. One window holds three wheels: the active bank at full size in the
 * middle, the previous and next banks at SMALL scale on either side (each scaled
 * about its own centre, without text). The window is rebuilt when the active bank
 * changes; selection changes on the active wheel are binds. */
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
#define SIZE 440.0f     /* window, GUI units */
#define RING 160.0f     /* ring radius */
#define ICON 48.0f
#define ICON_PICKED 68.0f
#define NAME_H 36.0f
#define PAD 28.0f       /* room for NUI's own padding (else it adds scrollbars) */
#define SMALL 0.5f      /* side wheels' scale */
#define GAP 24.0f       /* between wheels */
#define BANKS 3
#define WIDTH (SIZE + 2 * (SIZE * SMALL + GAP)) /* the window's content: three wheels */

static struct {
    bool open;
    int bank, selected, last_used; /* last_used: quickbar slot 0-35, -1 none */
    nwpad_qb_slot slots[NWPAD_QB_SLOTS];
} pk = {.selected = -1, .last_used = -1};

int nwpad_picker_selected(void) { return pk.open ? pk.selected : -1; }
bool nwpad_picker_open(void) { return pk.open; }
int nwpad_picker_last_used(void) { return pk.last_used; }

/* Active wheel centre in the window's content; side wheels left and right of it. */
static float wheel_x(int side) { return WIDTH / 2 + (float)side * (SIZE / 2 + GAP + SIZE * SMALL / 2); }

static void slot_centre_at(int i, float wx, float scale, float *x, float *y) {
    float a = ((float)i * 360.0f / NWPAD_PICKER_SLOTS - 90.0f) * NWPAD_RAD_PER_DEG;
    *x = wx + RING * scale * cosf(a);
    *y = SIZE / 2 + RING * scale * sinf(a);
}

static void slot_centre(int i, float *x, float *y) { slot_centre_at(i, wheel_x(0), 1.0f, x, y); }

static int rect_json(char *out, size_t cap, float cx, float cy, float size) {
    return snprintf(out, cap, "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", cx - size / 2, cy - size / 2, size, size);
}

static const nwpad_qb_slot *bank_slot(int bank, int i) { return &pk.slots[bank * NWPAD_PICKER_SLOTS + i]; }
static const nwpad_qb_slot *ring_slot(int i) { return bank_slot(pk.bank, i); }
static bool has_icon(const nwpad_qb_slot *s) { return s->type != 0 && (s->icon[0] || s->parts[0][0]); }

/* The screen's centre in GUI units. */
static void screen_centre(float *x, float *y) {
    nwpad_gui_size(x, y);
    *x /= 2;
    *y /= 2;
}

/* One wheel's draw-list items: disc, then each button. The active wheel (side 0)
 * binds its icon rects (the highlight enlarges them) and shows item names as text;
 * the side wheels are drawn at SMALL scale, without text. */
static size_t draw_wheel(char *json, size_t n, size_t cap, int bank, int side) {
    char esc[300], rect[96];
    float wx = wheel_x(side), scale = side ? SMALL : 1.0f, disc = (2 * RING + 100) * scale;
    if (n >= cap) return n;
    n += (size_t)snprintf(json + n, cap - n,
        "%s{\"type\":2,\"enabled\":true,\"color\":{\"r\":0,\"g\":0,\"b\":0,\"a\":150},\"fill\":true,"
        "\"line_thickness\":1.0,\"order\":-1,\"render\":0,\"arrayBinds\":false,"
        "\"rect\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}}",
        side == -1 ? "" : ",", wx - disc / 2, SIZE / 2 - disc / 2, disc, disc);
    for (int i = 0; i < NWPAD_PICKER_SLOTS && n < cap; i++) {
        const nwpad_qb_slot *s = bank_slot(bank, i);
        float x, y;
        slot_centre_at(i, wx, scale, &x, &y);
        rect_json(rect, sizeof rect, x, y, ICON * scale);
        if (s->type == 0 || (!has_icon(s) && side)) { /* empty (or an item name on a side wheel): a faint dot */
            n += (size_t)snprintf(json + n, cap - n,
                ",{\"type\":2,\"enabled\":true,\"color\":{\"r\":255,\"g\":255,\"b\":255,\"a\":40},\"fill\":false,"
                "\"line_thickness\":1.0,\"order\":1,\"render\":0,\"arrayBinds\":false,\"rect\":%s}", rect);
        } else if (has_icon(s)) { /* one image, or an item's parts bottom to top (F34) */
            const char *refs[3] = {s->icon[0] ? s->icon : s->parts[0], s->icon[0] ? "" : s->parts[1],
                                   s->icon[0] ? "" : s->parts[2]};
            char where[112];
            if (side) snprintf(where, sizeof where, "%s", rect);
            else snprintf(where, sizeof where, "{\"bind\":\"r%d\"}", i);
            for (int k = 0; k < 3 && n < cap; k++) {
                if (!refs[k][0]) continue;
                nwpad_json_escape(esc, sizeof esc, refs[k]);
                n += (size_t)snprintf(json + n, cap - n,
                    ",{\"type\":5,\"enabled\":true,\"color\":null,\"fill\":null,\"line_thickness\":null,\"order\":1,"
                    "\"render\":0,\"arrayBinds\":false,\"image\":\"%s\",\"rect\":%s,"
                    "\"image_aspect\":0,\"image_halign\":0,\"image_valign\":0}", esc, where);
            }
        } else { /* active wheel, no icon images: its name, small */
            char shortname[12];
            snprintf(shortname, sizeof shortname, "%s", s->name);
            nwpad_game_json(esc, sizeof esc, shortname);
            n += (size_t)snprintf(json + n, cap - n,
                ",{\"type\":4,\"enabled\":true,\"color\":{\"r\":230,\"g\":220,\"b\":190,\"a\":255},\"fill\":null,"
                "\"line_thickness\":null,\"order\":1,\"render\":0,\"arrayBinds\":false,"
                "\"rect\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":20.0},\"text\":\"%s\"}", x - ICON / 2 - 6, y - 10, ICON + 12, esc);
        }
    }
    return n;
}

static bool build(void) {
    static char json[49152];
    char rect[96];
    float cx, cy;
    screen_centre(&cx, &cy);
    size_t n = (size_t)snprintf(json, sizeof json,
        "{\"version\":1,\"title\":false,\"resizable\":false,\"collapsed\":false,\"closable\":false,"
        "\"transparent\":true,\"border\":false,\"accepts_input\":false,\"size_constraint\":null,"
        "\"edge_constraint\":null,\"font\":\"\",\"geometry\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f},"
        /* The wheels are the root column's draw list; its children put the name in
         * the middle of the active wheel: spacer, label, spacer. */
        "\"root\":{\"type\":\"col\",\"label\":null,\"value\":null,\"draw_list_scissor\":false,\"children\":["
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f},"
        "{\"type\":\"label\",\"label\":null,\"value\":{\"bind\":\"name\"},\"text_halign\":0,\"text_valign\":0,"
        "\"height\":%.1f},"
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f}],"
        "\"draw_list\":[",
        cx - (WIDTH + PAD) / 2, cy - (SIZE + PAD) / 2, WIDTH + PAD, SIZE + PAD, (SIZE - NAME_H) / 2, NAME_H,
        (SIZE - NAME_H) / 2);
    n = draw_wheel(json, n, sizeof json, (pk.bank + BANKS - 1) % BANKS, -1);
    n = draw_wheel(json, n, sizeof json, (pk.bank + 1) % BANKS, 1);
    n = draw_wheel(json, n, sizeof json, pk.bank, 0);
    if (n < sizeof json)
        n += (size_t)snprintf(json + n, sizeof json - n,
            ",{\"type\":2,\"enabled\":{\"bind\":\"hl_on\"},\"color\":{\"r\":235,\"g\":190,\"b\":80,\"a\":255},\"fill\":false,"
            "\"line_thickness\":3.0,\"order\":1,\"render\":0,\"arrayBinds\":false,\"rect\":{\"bind\":\"hl\"}}]}}");
    if (n >= sizeof json || !nwpad_nui_create(TOKEN, "nwpad_picker", json)) return false;
    for (int i = 0; i < NWPAD_PICKER_SLOTS; i++) {
        if (!has_icon(ring_slot(i))) continue; /* only icons have a rect bind */
        float x, y;
        slot_centre(i, &x, &y);
        rect_json(rect, sizeof rect, x, y, ICON);
        char bind[16];
        snprintf(bind, sizeof bind, "r%d", i);
        nwpad_nui_bind(TOKEN, bind, rect);
    }
    nwpad_nui_bind(TOKEN, "hl_on", "false");
    nwpad_nui_bind(TOKEN, "hl", "{\"x\":0.0,\"y\":0.0,\"w\":1.0,\"h\":1.0}");
    nwpad_nui_bind(TOKEN, "name", "\"\"");
    return true;
}

static void show_selection(int previous, int now) {
    char rect[96], bind[16], esc[300], value[320];
    float x, y;
    if (previous >= 0 && has_icon(ring_slot(previous))) {
        slot_centre(previous, &x, &y);
        rect_json(rect, sizeof rect, x, y, ICON);
        snprintf(bind, sizeof bind, "r%d", previous);
        nwpad_nui_bind(TOKEN, bind, rect);
    }
    slot_centre(now, &x, &y);
    if (has_icon(ring_slot(now))) {
        rect_json(rect, sizeof rect, x, y, ICON_PICKED);
        snprintf(bind, sizeof bind, "r%d", now);
        nwpad_nui_bind(TOKEN, bind, rect);
    }
    rect_json(rect, sizeof rect, x, y, ICON_PICKED + 10);
    nwpad_nui_bind(TOKEN, "hl", rect);
    nwpad_nui_bind(TOKEN, "hl_on", "true");
    const nwpad_qb_slot *s = ring_slot(now);
    nwpad_game_json(esc, sizeof esc, s->type ? s->name : "");
    snprintf(value, sizeof value, "\"%s\"", esc);
    nwpad_nui_bind(TOKEN, "name", value);
}

bool nwpad_picker_shift(int direction) {
    if (!pk.open) return false;
    pk.bank = (pk.bank + (direction < 0 ? BANKS - 1 : 1)) % BANKS;
    nwpad_nui_destroy(TOKEN);
    pk.open = build();
    if (pk.open && pk.selected >= 0) show_selection(-1, pk.selected); /* same position, new bank */
    return true;
}

int nwpad_picker_bank(void) { return pk.open ? pk.bank : -1; }

void nwpad_picker_close(bool use) {
    if (!pk.open) return;
    nwpad_nui_destroy(TOKEN);
    pk.open = false;
    if (use && pk.selected >= 0 && ring_slot(pk.selected)->type != 0 &&
        nwpad_quickbar_use(pk.bank * NWPAD_PICKER_SLOTS + pk.selected))
        pk.last_used = pk.bank * NWPAD_PICKER_SLOTS + pk.selected;
    pk.selected = -1;
}

bool nwpad_picker_frame(bool want, nwpad_vec2 right, bool in_game) {
    if (!pk.open) {
        if (!want || !in_game) return false;
        int bank = nwpad_quickbar_bank();
        if (bank < 0 || !nwpad_quickbar_read(pk.slots)) return false;
        pk.bank = bank;
        pk.selected = -1;
        pk.open = build();
        return pk.open;
    }
    if (want && in_game) {
        int now = nwpad_picker_select(right, pk.selected);
        if (now != pk.selected && now >= 0) show_selection(pk.selected, now);
        pk.selected = now;
        return true;
    }
    /* No longer wanted (cancelled, or out of the game): close without using. */
    nwpad_picker_close(false);
    return false;
}
