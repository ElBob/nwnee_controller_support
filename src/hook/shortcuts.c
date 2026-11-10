/* See shortcuts.h. One transparent NUI window: a disc, eight slots round it (the label
 * of each, in a box), and a highlight ring that is a bind. The wheel is rebuilt each
 * time it opens; the highlight and the centre name are binds. */
#include "shortcuts.h"

#include "backend.h"
#include "dialog.h"
#include "game.h"
#include "nui.h"
#include "sigs.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TOKEN (NWPAD_NUI_TOKEN_BASE + 0x110)
#define SIZE 380.0f   /* the wheel's height, GUI units */
#define WIDE (SIZE + 70.0f) /* the window's content width: room for the labels at the sides */
#define RING 135.0f   /* ring radius */
#define CHAR_W 7.5f   /* the default NUI font's advance, to centre a label (text is left-aligned in its box) */
#define BOX_H 30.0f
#define NAME_H 34.0f
#define PAD 28.0f     /* room for NUI's own padding */

static const struct { const char *label; int action; } SLOT[NWPAD_SHORTCUTS] = {
    {"Character", NWPAD_GAME_CHAR_SHEET}, {"Inventory", NWPAD_GAME_INVENTORY}, {"Spellbook", NWPAD_GAME_SPELLBOOK},
    {"Journal", NWPAD_GAME_JOURNAL},      {"Map", NWPAD_GAME_MAP},             {"Rest", NWPAD_GAME_REST},
    {"Party", NWPAD_GAME_PARTY},          {"Options", NWPAD_GAME_OPTIONS},
};

static struct {
    bool open;
    int selected;
#ifdef NWPAD_DEBUG_SURFACES
    int last_action;
#endif
} sc = {.selected = -1};

bool nwpad_shortcuts_open(void) { return sc.open; }

#ifdef NWPAD_DEBUG_SURFACES
int nwpad_shortcuts_selected(void) { return sc.open ? sc.selected : -1; }
int nwpad_shortcuts_last_action(void) { return sc.last_action; }
#endif

static void slot_centre(int i, float *x, float *y) {
    float a = ((float)i * 360.0f / NWPAD_SHORTCUTS - 90.0f) * NWPAD_RAD_PER_DEG;
    *x = WIDE / 2 + RING * cosf(a);
    *y = SIZE / 2 + RING * sinf(a);
}

static int box_json(char *out, size_t cap, float cx, float cy, float w, float h) {
    return snprintf(out, cap, "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", cx - w / 2, cy - h / 2, w, h);
}

static float label_w(int i) { return (float)strlen(SLOT[i].label) * CHAR_W; }

static bool build(void) {
    static char json[8192];
    float cx, cy, x, y;
    nwpad_gui_size(&cx, &cy);
    cx /= 2;
    cy /= 2;
    size_t n = (size_t)snprintf(json, sizeof json,
        "{\"version\":1,\"title\":false,\"resizable\":false,\"collapsed\":false,\"closable\":false,"
        "\"transparent\":true,\"border\":false,\"accepts_input\":false,\"size_constraint\":null,"
        "\"edge_constraint\":null,\"font\":\"\",\"geometry\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f},"
        "\"root\":{\"type\":\"col\",\"label\":null,\"value\":null,\"draw_list_scissor\":false,\"children\":["
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f},"
        "{\"type\":\"label\",\"label\":null,\"value\":{\"bind\":\"name\"},\"text_halign\":0,\"text_valign\":0,"
        "\"height\":%.1f},"
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f}],"
        "\"draw_list\":[{\"type\":2,\"enabled\":true,\"color\":{\"r\":0,\"g\":0,\"b\":0,\"a\":150},\"fill\":true,"
        "\"line_thickness\":1.0,\"order\":-1,\"render\":0,\"arrayBinds\":false,"
        "\"rect\":{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}}",
        cx - (WIDE + PAD) / 2, cy - (SIZE + PAD) / 2, WIDE + PAD, SIZE + PAD, (SIZE - NAME_H) / 2, NAME_H,
        (SIZE - NAME_H) / 2, WIDE / 2 - (RING + 70.0f), SIZE / 2 - (RING + 70.0f), 2 * (RING + 70.0f),
        2 * (RING + 70.0f));
    for (int i = 0; i < NWPAD_SHORTCUTS && n < sizeof json; i++) {
        char rect[96];
        slot_centre(i, &x, &y);
        box_json(rect, sizeof rect, x, y, label_w(i) + 4, BOX_H);
        n += (size_t)snprintf(json + n, sizeof json - n,
            ",{\"type\":4,\"enabled\":true,\"color\":{\"r\":230,\"g\":220,\"b\":190,\"a\":255},\"fill\":null,"
            "\"line_thickness\":null,\"order\":1,\"render\":0,\"arrayBinds\":false,\"rect\":%s,\"text\":\"%s\"}",
            rect, SLOT[i].label);
    }
    if (n < sizeof json)
        n += (size_t)snprintf(json + n, sizeof json - n,
            ",{\"type\":7,\"enabled\":{\"bind\":\"hl_on\"},\"color\":{\"r\":235,\"g\":190,\"b\":80,\"a\":255},\"fill\":false,"
            "\"line_thickness\":3.0,\"order\":0,\"render\":0,\"arrayBinds\":false,\"rect\":{\"bind\":\"hl\"}}]}}");
    if (n >= sizeof json || !nwpad_nui_create(TOKEN, "nwpad_shortcuts", json)) return false;
    nwpad_nui_bind(TOKEN, "hl_on", "false");
    nwpad_nui_bind(TOKEN, "hl", "{\"x\":0.0,\"y\":0.0,\"w\":1.0,\"h\":1.0}");
    nwpad_nui_bind(TOKEN, "name", "\"\"");
    return true;
}

static void show_selection(int now) {
    char rect[96], value[64];
    float x, y;
    slot_centre(now, &x, &y);
    box_json(rect, sizeof rect, x, y, label_w(now) + 20, BOX_H + 6);
    nwpad_nui_bind(TOKEN, "hl", rect);
    nwpad_nui_bind(TOKEN, "hl_on", "true");
    snprintf(value, sizeof value, "\"%s\"", SLOT[now].label);
    nwpad_nui_bind(TOKEN, "name", value);
}

void nwpad_shortcuts_close(bool use) {
    if (!sc.open) return;
    nwpad_nui_destroy(TOKEN);
    sc.open = false;
    int slot = sc.selected;
    sc.selected = -1;
    if (use && slot >= 0 && nwpad_backend_game_action(SLOT[slot].action)) {
#ifdef NWPAD_DEBUG_SURFACES
        sc.last_action = SLOT[slot].action;
#endif
    }
}

bool nwpad_shortcuts_frame(bool want, nwpad_vec2 right, bool in_game) {
    if (!sc.open) {
        if (!want || !in_game) return false;
        sc.selected = -1;
        sc.open = build();
        return sc.open;
    }
    if (want && in_game) {
        int now = nwpad_ring_select(right, sc.selected, NWPAD_SHORTCUTS);
        if (now != sc.selected && now >= 0) show_selection(now);
        sc.selected = now;
        return true;
    }
    nwpad_shortcuts_close(false);
    return false;
}
