/* See dialogui.h. The window is rebuilt for each new line/replies; the highlight
 * is colour binds. */
#include "dialogui.h"

#include "dialog.h"
#include "icons.h"
#include "game.h"
#include "sigs.h"

#define SEP_H 8.0f   /* the line under the NPC's text */
#define SCROLL_W 18.0f /* the text's scrollbar (the skin's scrollbar_size) */
#define BAR_TRIM 18.0f /* ends the scrollbar with the last line of text, not the text box */
#define WIN_X 2.0f   /* the window's place (geometry bind) */
#define WIN_Y 2.0f
#define ROWS 6       /* reply rows when they scroll (4 replies + "..." rows) */
#define ROW_GAP 4.0f /* NUI's spacing between rows */
#define QUICKBAR_H 60.0f /* keep clear of the quickbar at the bottom */
#include "nui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Four tokens in turn: a new window waits off screen for a few frames (the mouse,
 * F37) while the one it replaces stays up, and a window destroyed and recreated
 * under one token in consecutive frames didn't come back (the destroy lands after). */
#define TOKEN_BASE (NWPAD_NUI_TOKEN_BASE + 0x200)
#define SLOTS 4
#define TOKEN_OF(slot) (TOKEN_BASE + (slot))
#define TOKEN TOKEN_OF(ui.slot)
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
    bool preview;         /* debug: a made-up conversation is shown */
    int slot;             /* the newest window's token (TOKEN) */
    int shown;            /* the slot whose window is placed on screen (-1 none) */
    uint32_t frame, gone[SLOTS]; /* frame count; when each slot's window was destroyed */
    int first, last;      /* the replies shown (more than fit on screen scroll) */
    float rh[NWPAD_DIALOG_REPLIES]; /* each reply's row height */
    float room;           /* height available for the replies */
    int nrows, row_kind[ROWS + 2]; /* the rows: a reply index, or -1 for "..." */
    float row_h[ROWS + 2];
    int bound_kind[ROWS + 2], bound_colour[ROWS + 2]; /* what each row's binds hold (-2 nothing) */
    bool mouse;           /* the rows and the text take NUI's mouse input (F37) */
    int press;            /* the row a left button went down on (-1 none) */
    int mouse_tries;      /* frames spent waiting for the elements */
    bool dirty;           /* build the window again on the next frame */
    char wrapped[4200];   /* the NPC's line, wrapped by nwpad: lines separated by '\n' */
    int line_at[512], lines, top, visible; /* line starts in wrapped; the shown range */
    uint32_t seq;
    int highlight;
    nwpad_dialog d;
    float nav_dir;        /* the stick's held direction (-1, 0, +1) */
    uint64_t nav_next_ms; /* when a held stick steps again */
} ui = {.highlight = -1, .press = -1, .shown = -1};

#define TAG_TEXT 100 /* input tags: a row's index, or the NPC's text */

bool nwpad_dialogui_open(void) { return ui.open; }

#ifdef NWPAD_DEBUG_SURFACES
static struct {
    bool dry;                 /* clicks are recorded, not answered */
    nwpad_nui_input last;     /* the last mouse input */
    unsigned inputs, builds;  /* mouse inputs; windows built again (not for a new line) */
    unsigned place_frames, place_frames_max; /* frames from creation to placement */
    uint64_t build_ns, build_max_ns;         /* nwpad's own time in those builds */
} dbg;
#endif

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


static void trim(char *s) { /* leading and trailing blanks (the campaigns have many) */
    size_t a = strspn(s, " \t\r\n"), n = strlen(s);
    while (n > a && strchr(" \t\r\n", s[n - 1])) n--;
    memmove(s, s + a, n - a);
    s[n - a] = '\0';
}

/* Wrap the NPC's line into ui.wrapped at `cols` characters (word boundaries). */
static void wrap(const char *text, int cols) {
    size_t n = 0;
    ui.lines = 0;
    const char *p = text;
    while (*p && ui.lines < (int)(sizeof ui.line_at / sizeof ui.line_at[0])) {
        ui.line_at[ui.lines++] = (int)n;
        const char *end = p, *brk = NULL;
        int len = 0;
        while (*end && *end != '\n' && len < cols) {
            if (*end == ' ') brk = end;
            end++, len++;
        }
        if (*end && *end != '\n' && brk) end = brk; /* break at the last space */
        size_t take = (size_t)(end - p);
        if (n + take + 2 >= sizeof ui.wrapped) break;
        memcpy(ui.wrapped + n, p, take);
        n += take;
        ui.wrapped[n++] = '\n';
        p = end;
        if (*p == ' ' || *p == '\n') p++;
    }
    ui.wrapped[n ? n - 1 : 0] = '\0';
    if (!ui.lines) ui.line_at[ui.lines++] = 0;
}

/* A scrollbar beside the text, drawn with the skin's own scrollbar images (NUI's
 * only exists while NUI scrolls the text itself): arrows, track, and a thumb whose
 * place is a bind. */
static float bar_h;
static const char *scrollbar(float h) {
    static char out[2048];
    bar_h = h;
    static const char img[] = "{\"type\":5,\"enabled\":true,\"color\":null,\"fill\":null,\"line_thickness\":null,"
                              "\"order\":1,\"render\":0,\"arrayBinds\":false,\"image\":\"%s\",\"rect\":%s,"
                              "\"image_aspect\":5,\"image_halign\":0,\"image_valign\":0}";
    char a[400], b[400], c[400], d[400], r[96];
    snprintf(r, sizeof r, "{\"x\":0.0,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", SCROLL_W, SCROLL_W, h - 2 * SCROLL_W);
    snprintf(a, sizeof a, img, "nui_windowv", r);
    snprintf(r, sizeof r, "{\"x\":0.0,\"y\":0.0,\"w\":%.1f,\"h\":%.1f}", SCROLL_W, SCROLL_W);
    snprintf(b, sizeof b, img, "nui_cnt_up", r);
    snprintf(r, sizeof r, "{\"x\":0.0,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", h - SCROLL_W, SCROLL_W, SCROLL_W);
    snprintf(c, sizeof c, img, "nui_cnt_down", r);
    snprintf(d, sizeof d, img, "nui_scrollv", "{\"bind\":\"thumb\"}");
    snprintf(out, sizeof out,
             ",{\"type\":\"spacer\",\"label\":null,\"value\":null,\"width\":%.1f,\"height\":%.1f,"
             "\"draw_list_scissor\":false,\"draw_list\":[%s,%s,%s,%s]}", SCROLL_W, h, a, b, c, d);
    return out;
}

static void show_thumb(void) {
    if (ui.lines <= ui.visible) return;
    float track = bar_h - 2 * SCROLL_W, th = track * (float)ui.visible / (float)ui.lines;
    if (th < 20) th = 20;
    float y = SCROLL_W + (track - th) * (float)ui.top / (float)(ui.lines - ui.visible);
    char r[96];
    snprintf(r, sizeof r, "{\"x\":0.0,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", y, SCROLL_W, th);
    nwpad_nui_bind(TOKEN, "thumb", r);
}

/* Show lines [top, top + visible) of the NPC's line. */
static void show_body(void) {
    static char text[4200], esc[9000], value[9100];
    int from = ui.line_at[ui.top], last = ui.top + ui.visible;
    size_t to = last < ui.lines ? (size_t)ui.line_at[last] - 1 : strlen(ui.wrapped);
    snprintf(text, sizeof text, "%.*s", (int)(to - (size_t)from), ui.wrapped + from);
    nwpad_game_json(esc, sizeof esc, text);
    snprintf(value, sizeof value, "\"%s\"", esc);
    nwpad_nui_bind(TOKEN, "body", value);
    show_thumb();
}

static void reply_text(int i, char *out, size_t cap) {
    char numbered[600];
    snprintf(numbered, sizeof numbered, "%d. %s", i + 1, ui.d.replies[i].text);
    nwpad_game_json(out, cap, numbered);
}

/* Row k's text and colour (the rows are binds, so scrolling needn't rebuild). */
static void bind_row(int k) {
    static char esc[2400], value[2500];
    static const char *const colours[] = {"{\"r\":150,\"g\":150,\"b\":150,\"a\":255}",  /* ... */
                                          "{\"r\":128,\"g\":128,\"b\":128,\"a\":255}",  /* not selectable */
                                          "{\"r\":235,\"g\":190,\"b\":80,\"a\":255}",   /* highlighted */
                                          "{\"r\":125,\"g\":180,\"b\":255,\"a\":255}"}; /* a reply */
    char name[16];
    int i = ui.row_kind[k];
    int c = i < 0 ? 0 : !ui.d.replies[i].selectable ? 1 : i == ui.highlight ? 2 : 3;
    if (c != ui.bound_colour[k]) { /* only what changed: binds go through the game's NUI handler */
        snprintf(name, sizeof name, "c%d", k);
        nwpad_nui_bind(TOKEN, name, colours[c]);
        ui.bound_colour[k] = c;
    }
    if (i == ui.bound_kind[k]) return;
    ui.bound_kind[k] = i;
    if (i < 0) snprintf(esc, sizeof esc, "...");
    else reply_text(i, esc, sizeof esc);
    snprintf(value, sizeof value, "\"%s\"", esc);
    snprintf(name, sizeof name, "t%d", k);
    nwpad_nui_bind(TOKEN, name, value);
}

/* Which replies show, as rows: all if they fit, else ROWS rows around the highlight
 * with "..." in place of the first / last row when there are more that way. */
static void layout(void) {
    float total = 0, dots_h = LINE_H + 6 + ROW_GAP;
    for (int i = 0; i < ui.d.count; i++) total += ui.rh[i];
    ui.last = ui.d.count - 1;
    bool above = false, below = false;
    if (ui.d.count > ROWS || total > ui.room) {
        for (int rows = ROWS; rows >= 3; rows--) {
            if (ui.highlight >= 0 && ui.highlight < ui.first) ui.first = ui.highlight;
            for (;;) { /* lay out from ui.first; move down until the highlight shows */
                above = ui.first > 0;
                int slots = rows - (above ? 1 : 0);
                below = ui.first + slots < ui.d.count;
                if (below) slots--;
                ui.last = ui.first + slots - 1;
                if (ui.highlight <= ui.last || ui.last >= ui.d.count - 1) break;
                ui.first++;
            }
            while (!below && ui.first > 0 && (ui.last - ui.first + 1) + (ui.first > 1 ? 1 : 0) < rows) {
                ui.first--; /* at the end: fill the rows upwards */
                above = ui.first > 0;
            }
            float used = (above ? dots_h : 0) + (below ? dots_h : 0);
            for (int i = ui.first; i <= ui.last; i++) used += ui.rh[i];
            if (used <= ui.room) break; /* long replies: fewer rows */
        }
    } else {
        ui.first = 0;
    }
    ui.nrows = 0;
    if (above) { ui.row_kind[ui.nrows] = -1; ui.row_h[ui.nrows++] = dots_h; }
    for (int i = ui.first; i <= ui.last && ui.nrows < ROWS + 1; i++) { ui.row_kind[ui.nrows] = i; ui.row_h[ui.nrows++] = ui.rh[i]; }
    if (below) { ui.row_kind[ui.nrows] = -1; ui.row_h[ui.nrows++] = dots_h; }
}

/* Screen height in GUI units. */
static float screen_h(void) {
    float w, h;
    nwpad_gui_size(&w, &h);
    return h;
}

/* The mouse (F37). NUI only takes a window's input if the definition says so, and an
 * element with an id whose callbacks are still the game's would send its events to
 * the server. So the window is made off screen, every element with an id is taken
 * (the game builds them after the create message: tried again on the next frames),
 * and only then is it placed. If they can't be taken, the window is made again the
 * old way (no input, no ids) for the rest of the session. */
#define MOUSE_TRIES 30
static bool mouse_broken;
static void place(void);
static char placed[96]; /* the geometry bind for the window's place */

static void take_mouse(void) {
    if (ui.mouse || mouse_broken || ui.mouse_tries >= MOUSE_TRIES) return;
    ui.mouse_tries++;
    bool all = nwpad_nui_take_input(TOKEN, "line", TAG_TEXT);
    for (int k = 0; all && k < ui.nrows; k++) {
        char id[16];
        snprintf(id, sizeof id, "r%d", k);
        all = nwpad_nui_take_input(TOKEN, id, k);
    }
    if (all) {
        ui.mouse = true;
        place();
#ifdef NWPAD_DEBUG_SURFACES
        dbg.place_frames = (unsigned)ui.mouse_tries;
        if (dbg.place_frames > dbg.place_frames_max) dbg.place_frames_max = dbg.place_frames;
#endif
    } else if (ui.mouse_tries >= MOUSE_TRIES) {
        mouse_broken = true;
        ui.dirty = true; /* again, without input */
    }
}

static bool build(void) {
    static char json[65536];
    float text_w = WIDTH - PORTRAIT_W - 3 * PAD, char_w = big_font ? BIG_CHAR_W : CHAR_W,
          lh = big_font ? BIG_LINE_H : LINE_H;
    wrap(ui.d.line, (int)floorf(text_w / char_w));
    ui.visible = ui.lines;
    bool scrolls = ui.visible * lh > LINE_MAX_H;
    if (scrolls) { /* the sticks scroll the rest: make room for a scrollbar */
        wrap(ui.d.line, (int)floorf((text_w - SCROLL_W - 6) / char_w));
        ui.visible = (int)(LINE_MAX_H / lh);
    }
    if (ui.top > ui.lines - ui.visible) ui.top = ui.lines - ui.visible;
    if (ui.top < 0) ui.top = 0;
    float body_h = (float)ui.visible * lh + 6;
    float line_h = body_h + LINE_H + 4;
    float top = line_h > PORTRAIT_H ? line_h : PORTRAIT_H, h = top + PAD + SEP_H + ROW_GAP; /* + NUI padding */
    size_t n = 0;
    char name[400];
    nwpad_game_json(name, sizeof name, ui.d.speaker_name);
    n += (size_t)snprintf(json + n, sizeof json - n,
        /* (no row height: NUI's padding wouldn't fit around a full-height portrait) */
        "{\"type\":\"row\",\"label\":null,\"value\":null,\"children\":["
        /* portrait textures are 64x128 with the picture in the top 100 rows */
        "{\"type\":\"image\",\"label\":null,\"value\":\"%s\",\"image_aspect\":0,\"image_halign\":0,\"image_valign\":1,"
        "\"image_region\":{\"x\":0,\"y\":0,\"w\":64,\"h\":100},\"width\":%.1f,\"height\":%.1f},"
        "{\"type\":\"col\",\"label\":null,\"value\":null,\"children\":["
        "{\"type\":\"label\",\"label\":null,\"value\":\"%s\",\"text_halign\":1,\"text_valign\":0,\"height\":%.1f,"
        "\"foreground_color\":{\"r\":125,\"g\":180,\"b\":255,\"a\":255}},"
        "{\"type\":\"row\",\"label\":null,\"value\":null,\"children\":["
        "{\"type\":\"text\",\"label\":null,%s\"value\":{\"bind\":\"body\"},\"border\":false,\"scrollbars\":0,"
        "\"height\":%.1f%s}%s]}]}]},"
        /* a subtle line between the NPC's text and the replies */
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f,\"draw_list_scissor\":false,"
        "\"draw_list\":[{\"type\":6,\"enabled\":true,\"color\":{\"r\":173,\"g\":142,\"b\":96,\"a\":110},"
        "\"fill\":null,\"line_thickness\":1.0,\"order\":1,\"render\":0,\"arrayBinds\":false,"
        "\"a\":{\"x\":0.0,\"y\":%.1f},\"b\":{\"x\":%.1f,\"y\":%.1f}}]}",
        ui.d.portrait, PORTRAIT_W, PORTRAIT_H, name, LINE_H, mouse_broken ? "" : "\"id\":\"line\",", body_h, big_font ? ",\"font\":\"" FONT "\"" : "",
        scrolls ? scrollbar(body_h - BAR_TRIM) : "", SEP_H, SEP_H / 2, WIDTH - 2 * PAD, SEP_H / 2);
    ui.room = screen_h() - h - 2 * PAD - QUICKBAR_H;
    for (int i = 0; i < ui.d.count; i++) {
        char numbered[600];
        snprintf(numbered, sizeof numbered, "%d. %s", i + 1, ui.d.replies[i].text);
        ui.rh[i] = text_height(numbered, WIDTH - 2 * PAD) + ROW_GAP;
    }
    layout();
    for (int k = 0; k < ui.nrows && n < sizeof json; k++) {
        char id[24];
        snprintf(id, sizeof id, "\"id\":\"r%d\",", k);
        h += ui.row_h[k];
        n += (size_t)snprintf(json + n, sizeof json - n,
            ",{\"type\":\"text\",\"label\":null,%s\"value\":{\"bind\":\"t%d\"},\"border\":false,"
            "\"scrollbars\":0,\"height\":%.1f,\"foreground_color\":{\"bind\":\"c%d\"}}", mouse_broken ? "" : id, k,
            ui.row_h[k] - ROW_GAP, k);
    }
    if (ui.d.panel_h + 4 > h) h = ui.d.panel_h + 4; /* never smaller than the game's window */
    static char window[70000];
    int w = snprintf(window, sizeof window,
        "{\"version\":1,\"title\":false,\"resizable\":false,\"collapsed\":false,\"closable\":false,"
        "\"transparent\":false,\"border\":true,\"accepts_input\":%s,\"size_constraint\":null,"
        "\"edge_constraint\":null,\"font\":\"\",\"geometry\":{\"bind\":\"geo\"},"
        /* an opaque backdrop: the skin's window background lets the game's own
         * dialog window show through (and it highlights rows under the mouse) */
        "\"root\":{\"type\":\"col\",\"label\":null,\"value\":null,\"draw_list_scissor\":false,\"draw_list\":["
        "{\"type\":2,\"enabled\":true,\"color\":{\"r\":0,\"g\":0,\"b\":0,\"a\":255},\"fill\":true,\"line_thickness\":1.0,"
        "\"order\":-1,\"render\":0,\"arrayBinds\":false,\"rect\":{\"x\":-10.0,\"y\":-10.0,\"w\":%.1f,\"h\":%.1f}}],"
        "\"children\":[%s]}}",
        mouse_broken ? "false" : "true", WIDTH, h, json);
    if (w <= 0 || (size_t)w >= sizeof window || n >= sizeof json) return false;
    /* An id per slot, as the token: a new window whose id is still taken by the
     * closing one is placed elsewhere (centred) by NUI. */
    char wid[24];
    snprintf(wid, sizeof wid, "nwpad_dialog_%d", ui.slot);
    if (!nwpad_nui_create(TOKEN, wid, window)) return false;
    /* Placed by a bind: a window created right after another one closes was
     * sometimes centred by NUI when the geometry was in the definition. */
    snprintf(placed, sizeof placed, "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", WIN_X, WIN_Y, WIDTH, h);
    ui.mouse = false;
    ui.mouse_tries = 0;
    ui.press = -1;
    if (mouse_broken) {
        place();
    } else { /* off screen until its elements are nwpad's */
        char away[96];
        snprintf(away, sizeof away, "{\"x\":-10000.0,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", WIN_Y, WIDTH, h);
        nwpad_nui_bind(TOKEN, "geo", away);
        take_mouse();
    }
    for (int k = 0; k < ROWS + 2; k++) ui.bound_kind[k] = ui.bound_colour[k] = -2;
    for (int k = 0; k < ui.nrows; k++) bind_row(k);
    show_body();
    return true;
}

void nwpad_dialogui_scroll(int lines) {
    if (!ui.open) return;
    int top = ui.top + lines, max = ui.lines - ui.visible;
    if (top > max) top = max;
    if (top < 0) top = 0;
    if (top == ui.top) return;
    ui.top = top;
    show_body();
}

static void drop(int slot) {
    nwpad_nui_destroy(TOKEN_OF(slot));
    ui.gone[slot] = ui.frame;
}

/* The new window is on screen: the one it replaces goes. */
static void place(void) {
    nwpad_nui_bind(TOKEN, "geo", placed);
    if (ui.shown >= 0 && ui.shown != ui.slot) drop(ui.shown);
    ui.shown = ui.slot;
}

/* Both windows (the shown one and a newer one still off screen). */
static void close_windows(void) {
    if (ui.open) drop(ui.slot);
    if (ui.shown >= 0 && ui.shown != ui.slot) drop(ui.shown);
    ui.shown = -1;
    ui.open = false;
}

/* A new window for the current state; the shown one stays until it's placed. A
 * newer one not yet placed is dropped (it was never seen). */
static void replace(void) {
    if (ui.open && ui.slot != ui.shown) drop(ui.slot);
    int next = -1;
    for (int i = 1; i <= SLOTS && next < 0; i++) {
        int s = (ui.slot + i) % SLOTS;
        if (s != ui.shown && ui.frame - ui.gone[s] >= 2) next = s;
    }
    if (next < 0) next = (ui.slot + 1) % SLOTS == ui.shown ? (ui.slot + 2) % SLOTS : (ui.slot + 1) % SLOTS; /* never expected */
    ui.slot = next;
    ui.dirty = false;
    ui.open = build();
    if (!ui.open && ui.shown >= 0) {
        drop(ui.shown);
        ui.shown = -1;
    }
}

/* The same line and replies in a new window (other row heights, or no mouse): once
 * per frame at most, however many steps asked for it. */
static void rebuild(void) {
#ifdef NWPAD_DEBUG_SURFACES
    uint64_t t0 = nwpad_now_ns(), g0 = nwpad_nui_take_game_ns();
    replace();
    uint64_t game = nwpad_nui_take_game_ns(), own = nwpad_now_ns() - t0 - game;
    nwpad_game_ns_add(g0 + game); /* put the game time back for the frame's account */
    dbg.builds++;
    dbg.build_ns += own;
    if (own > dbg.build_max_ns) dbg.build_max_ns = own;
#else
    replace();
#endif
}

static int first_selectable(void) {
    for (int i = 0; i < ui.d.count; i++)
        if (ui.d.replies[i].selectable) return i;
    return ui.d.count ? 0 : -1;
}

void nwpad_dialogui_move(int step) {
    if (!ui.open || ui.d.count == 0) return;
    int i = ui.highlight;
    for (int k = 0; k < ui.d.count; k++) { /* next selectable, wrapping */
        i = ((i < 0 ? 0 : i) + step + ui.d.count) % ui.d.count;
        if (ui.d.replies[i].selectable) break;
    }
    ui.highlight = i;
    int nrows = ui.nrows, kind[ROWS + 2];
    float heights[ROWS + 2];
    memcpy(kind, ui.row_kind, sizeof kind);
    memcpy(heights, ui.row_h, sizeof heights);
    layout();
    bool same = ui.nrows == nrows;
    for (int k = 0; same && k < nrows; k++) same = heights[k] == ui.row_h[k];
    if (!same) ui.dirty = true; /* other row heights (a long reply came into view): build again */
    if (ui.dirty) return;
    for (int k = 0; k < ui.nrows; k++) bind_row(k); /* scrolled or not: just the binds */
}

/* Answer with the reply in that row ("...": the next reply that way). */
static void click(int row) {
    if (!ui.open || row < 0 || row >= ui.nrows) return;
    int i = ui.row_kind[row];
    if (i < 0) { /* "...": the next reply that way */
        nwpad_dialogui_move(row == 0 ? -1 : 1);
        return;
    }
    if (!ui.d.replies[i].selectable) return;
    ui.highlight = i;
    nwpad_dialogui_confirm();
}

/* NUI's mouse input on the window (taken in the game's NUI pass, F37): a press and
 * release on the same row answers with it; the wheel scrolls the text over the
 * text, else moves the highlight. Input on a window being replaced (for the few
 * frames its successor waits off screen) is dropped: its rows may differ. */
static void mouse_input(void) {
    nwpad_nui_input in;
    bool dry = false;
#ifdef NWPAD_DEBUG_SURFACES
    dry = dbg.dry;
#endif
    while (nwpad_nui_next_input(&in)) {
        if (in.token != TOKEN || !ui.open || nwpad_dialog_seq() != ui.seq) continue;
#ifdef NWPAD_DEBUG_SURFACES
        dbg.last = in;
        dbg.inputs++;
#endif
        if (in.kind == NWPAD_NUI_SCROLL && in.y != 0) {
            if (in.tag == TAG_TEXT) nwpad_dialogui_scroll(in.y > 0 ? -3 : 3); /* wheel up: earlier lines */
            else nwpad_dialogui_move(in.y > 0 ? -1 : 1);
        } else if (in.button == 0 && in.kind == NWPAD_NUI_DOWN) {
            ui.press = in.tag;
        } else if (in.button == 0 && in.kind == NWPAD_NUI_UP) {
            int row = ui.press;
            ui.press = -1;
            if (row == in.tag && row != TAG_TEXT && !dry) click(row);
        }
    }
}

void nwpad_dialogui_confirm(void) {
    if (ui.open && ui.highlight >= 0 && !ui.preview) nwpad_dialog_select(ui.highlight);
}

void nwpad_dialogui_cancel(void) {
    if (ui.open && !ui.preview) nwpad_dialog_end();
}

#ifdef NWPAD_DEBUG_SURFACES
static void unescape(char *s) { /* "\n" -> newline, in place */
    char *o = s;
    for (char *p = s; *p; p++) {
        if (p[0] == '\\' && p[1] == 'n') { *o++ = '\n'; p++; }
        else *o++ = *p;
    }
    *o = '\0';
}

bool nwpad_dialogui_preview(const char *path) {
    close_windows();
    ui.preview = false;
    if (!path) return true;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    memset(&ui.d, 0, sizeof ui.d);
    static char buf[8192];
    int n = 0;
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\n")] = '\0';
        unescape(buf);
        if (n == 0) snprintf(ui.d.line, sizeof ui.d.line, "%.4000s", buf);
        else if (ui.d.count < NWPAD_DIALOG_REPLIES) {
            snprintf(ui.d.replies[ui.d.count].text, sizeof ui.d.replies[0].text, "%.500s", buf);
            ui.d.replies[ui.d.count++].selectable = true;
        }
        n++;
    }
    fclose(f);
    snprintf(ui.d.speaker_name, sizeof ui.d.speaker_name, "%s", "Preview");
    snprintf(ui.d.portrait, sizeof ui.d.portrait, "%s", "po_dw_m_01_M");
    ui.d.panel_h = 254;
    ui.highlight = first_selectable();
    ui.first = ui.top = 0;
    replace();
    ui.preview = ui.open;
    return ui.open;
}
#endif

bool nwpad_dialogui_frame(bool enabled, float nav, uint64_t now_ms) {
    ui.frame++;
    mouse_input(); /* last frame's, on the window as it was shown */
    if (ui.preview) {
        if (ui.dirty) rebuild();
        take_mouse();
        return true;
    }
    uint32_t seq = enabled ? nwpad_dialog_seq() : 0;
    if (!seq) {
        close_windows();
        ui.highlight = -1;
        return false;
    }
    if (!ui.open || seq != ui.seq) { /* a new line or new replies */
        ui.seq = seq;
        if (!nwpad_dialog_read(&ui.d)) return false;
        nwpad_strip_colour_codes(ui.d.line); /* no inline colours in NUI text */
        trim(ui.d.line);
        for (int i = 0; i < ui.d.count; i++) {
            nwpad_strip_colour_codes(ui.d.replies[i].text);
            trim(ui.d.replies[i].text);
        }
        ui.first = 0;
        ui.top = 0;
        ui.highlight = first_selectable();
        replace();
        if (!ui.open) return false;
    }
    if (ui.dirty) rebuild();
    take_mouse();
    /* The sticks scroll the NPC's text: a held stick steps a line, then repeats. */
    float dir = nav > 0.5f ? -1.0f : nav < -0.5f ? 1.0f : 0.0f; /* stick up = earlier lines */
    if (dir != ui.nav_dir) {
        ui.nav_dir = dir;
        if (dir != 0) {
            nwpad_dialogui_scroll((int)dir);
            ui.nav_next_ms = now_ms + 300;
        }
    } else if (dir != 0 && now_ms >= ui.nav_next_ms) {
        nwpad_dialogui_scroll((int)dir);
        ui.nav_next_ms = now_ms + 90;
    }
    return true;
}

#ifdef NWPAD_DEBUG_SURFACES
void nwpad_dialogui_debug_dry(bool dry) { dbg.dry = dry; }

void nwpad_dialogui_debug_json(char *out, size_t cap) {
    snprintf(out, cap,
             "\"open\":%s,\"highlight\":%d,\"first\":%d,\"last\":%d,\"text_top\":%d,\"rebuilds\":%u,"
             "\"rebuild_avg_us\":%.1f,\"rebuild_max_us\":%.1f,\"mouse\":%s,\"inputs\":%u,"
             "\"last_input\":{\"tag\":%d,\"kind\":%d,\"button\":%d,\"x\":%.1f,\"y\":%.1f},"
             "\"place_frames\":%u,\"place_frames_max\":%u",
             ui.open ? "true" : "false", ui.open ? ui.highlight : -1, ui.open ? ui.first : -1, ui.open ? ui.last : -1,
             ui.open ? ui.top : -1, dbg.builds, dbg.builds ? (double)dbg.build_ns / dbg.builds / 1000.0 : 0.0,
             (double)dbg.build_max_ns / 1000.0, ui.open && ui.mouse ? "true" : "false", dbg.inputs, dbg.last.tag,
             dbg.last.kind, dbg.last.button, dbg.last.x, dbg.last.y, dbg.place_frames, dbg.place_frames_max);
}
#endif
