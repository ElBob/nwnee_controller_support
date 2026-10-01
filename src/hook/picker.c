/* See picker.h. The window is built once per opening; selection changes are binds. */
#include "picker.h"

#include "nui.h"
#include "quickbar.h"
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
#define PLATE_W 200.0f /* the dark plate behind the name */
#define PAD 28.0f       /* room for NUI's own padding (else it adds scrollbars) */

static struct {
    bool open;
    int bank, selected, last_used; /* last_used: quickbar slot 0-35, -1 none */
    nwpad_qb_slot slots[NWPAD_QB_SLOTS];
} pk = {.selected = -1, .last_used = -1};

int nwpad_picker_selected(void) { return pk.open ? pk.selected : -1; }
bool nwpad_picker_open(void) { return pk.open; }
int nwpad_picker_last_used(void) { return pk.last_used; }

static void slot_centre(int i, float *x, float *y) {
    float a = ((float)i * 360.0f / NWPAD_PICKER_SLOTS - 90.0f) * (float)M_PI / 180.0f;
    *x = SIZE / 2 + RING * cosf(a);
    *y = SIZE / 2 + RING * sinf(a);
}

static int rect_json(char *out, size_t cap, float cx, float cy, float size) {
    return snprintf(out, cap, "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", cx - size / 2, cy - size / 2, size, size);
}

static const nwpad_qb_slot *ring_slot(int i) { return &pk.slots[pk.bank * NWPAD_PICKER_SLOTS + i]; }
static bool has_icon(const nwpad_qb_slot *s) { return s->type != 0 && (s->icon[0] || s->parts[0][0]); }

/* Window centre in GUI units: the screen is g_pGuiMan's size over the GUI scale. */
static void screen_centre(float *x, float *y) {
    float (*scale)(void) = (float (*)(void))nwpad_sig(NWPAD_SIG_GUI_SCALE);
    void **gui = (void **)nwpad_sig(NWPAD_SIG_GUI_MANAGER);
    float s = scale ? scale() : 1.0f;
    int w = gui && *gui ? *(int *)((char *)*gui + 0xb8) : 1280, h = gui && *gui ? *(int *)((char *)*gui + 0xbc) : 720;
    if (s <= 0) s = 1.0f;
    *x = (float)w / s / 2;
    *y = (float)h / s / 2;
}

static bool build(void) {
    static char json[24576];
    char esc[300], rect[96];
    float cx, cy;
    screen_centre(&cx, &cy);
    size_t n = (size_t)snprintf(json, sizeof json,
        "{\"version\":1,\"title\":false,\"resizable\":false,\"collapsed\":false,\"closable\":false,"
        "\"transparent\":true,\"border\":false,\"accepts_input\":false,\"size_constraint\":null,"
        "\"edge_constraint\":null,\"font\":\"\",\"geometry\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f},"
        /* The ring is the root column's draw list; its children put the name in the
         * middle: spacer, label, spacer. The dark disc is drawn before them. */
        "\"root\":{\"type\":\"col\",\"label\":null,\"value\":null,\"draw_list_scissor\":false,\"children\":["
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f},"
        "{\"type\":\"label\",\"label\":null,\"value\":{\"bind\":\"name\"},\"text_halign\":0,\"text_valign\":0,"
        "\"height\":%.1f},"
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f}],"
        "\"draw_list\":["
        "{\"type\":2,\"enabled\":true,\"color\":{\"r\":0,\"g\":0,\"b\":0,\"a\":150},\"fill\":true,\"line_thickness\":1.0,"
        "\"order\":-1,\"render\":0,\"arrayBinds\":false,\"rect\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}},"
        /* a darker plate behind the name: the character stands at the screen centre */
        "{\"type\":7,\"enabled\":{\"bind\":\"hl_on\"},\"color\":{\"r\":0,\"g\":0,\"b\":0,\"a\":200},\"fill\":true,"
        "\"line_thickness\":1.0,\"order\":-1,\"render\":0,\"arrayBinds\":false,"
        "\"rect\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}}",
        cx - (SIZE + PAD) / 2, cy - (SIZE + PAD) / 2, SIZE + PAD, SIZE + PAD, (SIZE - NAME_H) / 2, NAME_H,
        (SIZE - NAME_H) / 2, SIZE / 2 - RING - 50, SIZE / 2 - RING - 50, 2 * RING + 100, 2 * RING + 100,
        SIZE / 2 - PLATE_W / 2, SIZE / 2 - NAME_H / 2 - 2, PLATE_W, NAME_H);
    for (int i = 0; i < NWPAD_PICKER_SLOTS && n < sizeof json; i++) {
        const nwpad_qb_slot *s = ring_slot(i);
        float x, y;
        slot_centre(i, &x, &y);
        rect_json(rect, sizeof rect, x, y, ICON);
        if (s->type == 0) {  /* empty: a faint dot */
            n += (size_t)snprintf(json + n, sizeof json - n,
                ",{\"type\":2,\"enabled\":true,\"color\":{\"r\":255,\"g\":255,\"b\":255,\"a\":40},\"fill\":false,"
                "\"line_thickness\":1.0,\"order\":1,\"render\":0,\"arrayBinds\":false,\"rect\":%s}", rect);
        } else if (has_icon(s)) { /* one image, or an item's parts bottom to top (F34) */
            const char *refs[3] = {s->icon[0] ? s->icon : s->parts[0], s->icon[0] ? "" : s->parts[1],
                                   s->icon[0] ? "" : s->parts[2]};
            for (int k = 0; k < 3 && n < sizeof json; k++) {
                if (!refs[k][0]) continue;
                nwpad_json_escape(esc, sizeof esc, refs[k]);
                n += (size_t)snprintf(json + n, sizeof json - n,
                    ",{\"type\":5,\"enabled\":true,\"color\":null,\"fill\":null,\"line_thickness\":null,\"order\":1,"
                    "\"render\":0,\"arrayBinds\":false,\"image\":\"%s\",\"rect\":{\"bind\":\"r%d\"},"
                    "\"image_aspect\":0,\"image_halign\":0,\"image_valign\":0}", esc, i);
            }
        } else {  /* no icon images (armor): its name, small */
            char shortname[12];
            snprintf(shortname, sizeof shortname, "%s", s->name);
            nwpad_json_escape(esc, sizeof esc, shortname);
            n += (size_t)snprintf(json + n, sizeof json - n,
                ",{\"type\":4,\"enabled\":true,\"color\":{\"r\":230,\"g\":220,\"b\":190,\"a\":255},\"fill\":null,"
                "\"line_thickness\":null,\"order\":1,\"render\":0,\"arrayBinds\":false,"
                "\"rect\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":20.0},\"text\":\"%s\"}", x - ICON / 2 - 6, y - 10, ICON + 12, esc);
        }
    }
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
    nwpad_json_escape(esc, sizeof esc, s->type ? s->name : "");
    snprintf(value, sizeof value, "\"%s\"", esc);
    nwpad_nui_bind(TOKEN, "name", value);
}

bool nwpad_picker_frame(bool held, nwpad_vec2 right, bool in_game) {
    if (!pk.open) {
        if (!held || !in_game) return false;
        int bank = nwpad_quickbar_bank();
        if (bank < 0 || !nwpad_quickbar_read(pk.slots)) return false;
        pk.bank = bank;
        pk.selected = -1;
        pk.open = build();
        return pk.open;
    }
    if (held && in_game) {
        int now = nwpad_picker_select(right, pk.selected);
        if (now != pk.selected && now >= 0) show_selection(pk.selected, now);
        pk.selected = now;
        return true;
    }
    /* Released: close, and use the highlighted button (if any, and not empty). */
    nwpad_nui_destroy(TOKEN);
    pk.open = false;
    if (in_game && pk.selected >= 0 && ring_slot(pk.selected)->type != 0 &&
        nwpad_quickbar_use(pk.bank * NWPAD_PICKER_SLOTS + pk.selected))
        pk.last_used = pk.bank * NWPAD_PICKER_SLOTS + pk.selected;
    pk.selected = -1;
    return false;
}
