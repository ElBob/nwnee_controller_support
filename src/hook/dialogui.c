/* See dialogui.h. The window is rebuilt for each new line/replies; the highlight
 * is colour binds. */
#include "dialogui.h"

#include "dialog.h"
#include "icons.h"
#include "sigs.h"
#include "nui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Two tokens, alternated per rebuild: a window destroyed and recreated under one
 * token in consecutive frames didn't come back (the destroy lands after). */
#define TOKEN_BASE (NWPAD_NUI_TOKEN_BASE + 0x200)
#define TOKEN (TOKEN_BASE + ui.flip)
#define WIDTH 360.0f      /* covers the game's window (~350 wide at (5,5), F36) */
#define PORTRAIT_W 64.0f
#define PORTRAIT_H 100.0f
#define LINE_H 20.0f      /* default NUI font (17 px) line */
#define CHAR_W 7.6f       /* its average advance, for wrapping estimates */
#define PAD 12.0f
#define FONT "nwpad_dialog" /* the NPC's line: the game's main font, larger */
#define BIG_LINE_H 27.0f
#define BIG_CHAR_W 11.2f
#define LINE_MAX_H 380.0f /* longer NPC lines scroll */

static struct {
    bool open;
    int flip;             /* which of the two tokens is current */
    uint32_t seq;
    int highlight;
    nwpad_dialog d;
    float nav_dir;        /* the stick's held direction (-1, 0, +1) */
    uint64_t nav_next_ms; /* when a held stick steps again */
} ui = {.highlight = -1};

bool nwpad_dialogui_open(void) { return ui.open; }
int nwpad_dialogui_highlight(void) { return ui.open ? ui.highlight : -1; }

static bool big_font; /* the skin has FONT */

void nwpad_dialogui_setup_font(void) {
    size_t n = 0;
    uint8_t *skin = nwpad_resource_get("nui_skin", 2076 /* TML */, &n);
    void (*reload)(void) = (void (*)(void))nwpad_sig(NWPAD_SIG_NUI_RELOAD_SKIN);
    if (!skin || !reload) {
        free(skin);
        return;
    }
    char *text = malloc(n + 1);
    if (!text) { free(skin); return; }
    memcpy(text, skin, n);
    text[n] = '\0';
    free(skin);
    if (strstr(text, "\"" FONT "\"")) { /* ours already (served by us) */
        big_font = true;
        free(text);
        reload();
        return;
    }
    /* After the last [[fonts]] entry: the next top-level table. */
    char *last = NULL;
    for (char *p = strstr(text, "[[fonts]]"); p; p = strstr(p + 1, "[[fonts]]")) last = p;
    char *after = last ? strstr(last + 9, "\n[") : NULL;
    if (!after) { free(text); return; }
    static const char entry[] = "\n[[fonts]]\n  name = \"" FONT "\"\n  resref = \"fnt_maintext\"\n  pixel_height = 0.0\n"
                                "  pixel_snap = true\n  oversample_h = 1.0\n  oversample_v = 1.0\n  font_size = 22.0\n"
                                "  spacing_h = 0.5\n  spacing_v = 0.0\n";
    size_t head = (size_t)(after - text), total = n + sizeof entry - 1;
    char *out = malloc(total);
    if (out) {
        memcpy(out, text, head);
        memcpy(out + head, entry, sizeof entry - 1);
        memcpy(out + head + sizeof entry - 1, text + head, n - head);
        if (nwpad_resource_publish("nui_skin.tml", out, total)) {
            reload();
            big_font = true;
        }
        free(out);
    }
    free(text);
}

static float text_height_at(const char *text, float width, float char_w, float line_h) {
    float lines = 0, chars_per_line = floorf(width / char_w);
    for (const char *p = text; *p;) { /* per paragraph */
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        lines += len ? ceilf((float)len / chars_per_line) : 1;
        p += len + (nl ? 1 : 0);
    }
    return (lines < 1 ? 1 : lines) * line_h + 6;
}

static float text_height(const char *text, float width) { return text_height_at(text, width, CHAR_W, LINE_H); }

static void colour(int i) {
    char name[16];
    snprintf(name, sizeof name, "c%d", i);
    const char *c = !ui.d.replies[i].selectable  ? "{\"r\":128,\"g\":128,\"b\":128,\"a\":255}"
                    : i == ui.highlight          ? "{\"r\":235,\"g\":190,\"b\":80,\"a\":255}"
                                                 : "{\"r\":125,\"g\":180,\"b\":255,\"a\":255}";
    nwpad_nui_bind(TOKEN, name, c);
}

static bool build(void) {
    static char json[65536];
    static char esc[9000];
    float text_w = WIDTH - PORTRAIT_W - 3 * PAD,
          body_h = big_font ? text_height_at(ui.d.line, text_w, BIG_CHAR_W, BIG_LINE_H) : text_height(ui.d.line, text_w);
    bool scroll = body_h > LINE_MAX_H;
    if (scroll) body_h = LINE_MAX_H;
    float line_h = body_h + LINE_H + 4;
    float top = line_h > PORTRAIT_H ? line_h : PORTRAIT_H, h = top + 3 * PAD; /* + NUI padding */
    size_t n = 0;
    char name[400];
    nwpad_json_escape(name, sizeof name, ui.d.speaker_name);
    nwpad_json_escape(esc, sizeof esc, ui.d.line);
    n += (size_t)snprintf(json + n, sizeof json - n,
        /* (no row height: NUI's padding wouldn't fit around a full-height portrait) */
        "{\"type\":\"row\",\"label\":null,\"value\":null,\"children\":["
        /* portrait textures are 64x128 with the picture in the top 100 rows */
        "{\"type\":\"image\",\"label\":null,\"value\":\"%s\",\"image_aspect\":0,\"image_halign\":0,\"image_valign\":1,"
        "\"image_region\":{\"x\":0,\"y\":0,\"w\":64,\"h\":100},\"width\":%.1f,\"height\":%.1f},"
        "{\"type\":\"col\",\"label\":null,\"value\":null,\"children\":["
        "{\"type\":\"label\",\"label\":null,\"value\":\"%s\",\"text_halign\":1,\"text_valign\":0,\"height\":%.1f,"
        "\"foreground_color\":{\"r\":125,\"g\":180,\"b\":255,\"a\":255}},"
        "{\"type\":\"text\",\"label\":null,\"value\":\"%s\",\"border\":false,\"scrollbars\":%d,\"height\":%.1f%s}]}]}",
        ui.d.portrait, PORTRAIT_W, PORTRAIT_H, name, LINE_H, esc, scroll ? 2 : 0, body_h,
        big_font ? ",\"font\":\"" FONT "\"" : "");
    for (int i = 0; i < ui.d.count && n < sizeof json; i++) {
        char numbered[600];
        snprintf(numbered, sizeof numbered, "%d. %s", i + 1, ui.d.replies[i].text);
        nwpad_json_escape(esc, sizeof esc, numbered);
        float rh = text_height(numbered, WIDTH - 2 * PAD);
        h += rh + 8;
        n += (size_t)snprintf(json + n, sizeof json - n,
            ",{\"type\":\"text\",\"label\":null,\"value\":\"%s\",\"border\":false,\"scrollbars\":0,\"height\":%.1f,"
            "\"foreground_color\":{\"bind\":\"c%d\"}}", esc, rh, i);
    }
    if (ui.d.panel_h + 8 > h) h = ui.d.panel_h + 8; /* never smaller than the game's window */
    static char window[70000];
    int w = snprintf(window, sizeof window,
        "{\"version\":1,\"title\":false,\"resizable\":false,\"collapsed\":false,\"closable\":false,"
        "\"transparent\":false,\"border\":true,\"accepts_input\":false,\"size_constraint\":null,"
        "\"edge_constraint\":null,\"font\":\"\",\"geometry\":{\"bind\":\"geo\"},"
        "\"root\":{\"type\":\"col\",\"label\":null,\"value\":null,\"children\":[%s]}}",
        json);
    if (w <= 0 || (size_t)w >= sizeof window || n >= sizeof json) return false;
#ifdef NWPAD_DEBUG_SURFACES
    FILE *f = fopen("/tmp/nwpad_dialog.json", "w"); /* research: the last definition */
    if (f) { fputs(window, f); fclose(f); }
#endif
    /* The id alternates with the token: a new window whose id is still taken by the
     * closing one is placed elsewhere (centred) by NUI. */
    if (!nwpad_nui_create(TOKEN, ui.flip ? "nwpad_dialog_b" : "nwpad_dialog_a", window)) return false;
    /* Placed by a bind: a window created right after another one closes was
     * sometimes centred by NUI when the geometry was in the definition. */
    char geo[96];
    snprintf(geo, sizeof geo, "{\"x\":2.0,\"y\":2.0,\"w\":%.1f,\"h\":%.1f}", WIDTH, h + PAD);
    nwpad_nui_bind(TOKEN, "geo", geo);
    for (int i = 0; i < ui.d.count; i++) colour(i);
    return true;
}

static int first_selectable(void) {
    for (int i = 0; i < ui.d.count; i++)
        if (ui.d.replies[i].selectable) return i;
    return ui.d.count ? 0 : -1;
}

void nwpad_dialogui_move(int step) {
    if (!ui.open || ui.d.count == 0) return;
    int was = ui.highlight, i = ui.highlight;
    for (int k = 0; k < ui.d.count; k++) { /* next selectable, wrapping */
        i = ((i < 0 ? 0 : i) + step + ui.d.count) % ui.d.count;
        if (ui.d.replies[i].selectable) break;
    }
    ui.highlight = i;
    if (was >= 0) colour(was);
    colour(i);
}

void nwpad_dialogui_confirm(void) {
    if (ui.open && ui.highlight >= 0) nwpad_dialog_select(ui.highlight);
}

void nwpad_dialogui_cancel(void) {
    if (ui.open) nwpad_dialog_end();
}

bool nwpad_dialogui_frame(bool enabled, float nav, uint64_t now_ms) {
    uint32_t seq = enabled ? nwpad_dialog_seq() : 0;
    if (!seq) {
        if (ui.open) nwpad_nui_destroy(TOKEN);
        ui.open = false;
        ui.highlight = -1;
        return false;
    }
    if (!ui.open || seq != ui.seq) { /* a new line or new replies */
        ui.seq = seq;
        if (!nwpad_dialog_read(&ui.d)) return false;
        if (ui.open) {
            nwpad_nui_destroy(TOKEN);
            ui.flip ^= 1;
        }
        ui.highlight = first_selectable();
        ui.open = build();
        if (!ui.open) return false;
    }
    /* A held stick steps once, then repeats. */
    float dir = nav > 0.5f ? -1.0f : nav < -0.5f ? 1.0f : 0.0f; /* stick up = previous reply */
    if (dir != ui.nav_dir) {
        ui.nav_dir = dir;
        if (dir != 0) {
            nwpad_dialogui_move((int)dir);
            ui.nav_next_ms = now_ms + 400;
        }
    } else if (dir != 0 && now_ms >= ui.nav_next_ms) {
        nwpad_dialogui_move((int)dir);
        ui.nav_next_ms = now_ms + 150;
    }
    return true;
}
