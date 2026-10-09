/* See dialogui.h. The window is rebuilt for each new line/replies; the highlight
 * is colour binds. */
#include "dialogui.h"

#include "dialog.h"
#include "icons.h"
#include "sigs.h"

#define SEP_H 8.0f   /* the line under the NPC's text */
#define SCROLL_W 18.0f /* the text's scrollbar (the skin's scrollbar_size) */
#define BAR_TRIM 18.0f /* ends the scrollbar with the last line of text, not the text box */
#define WIN_X 2.0f   /* the window's place (geometry bind) */
#define WIN_Y 2.0f
#define HIT_TOP 22.0f /* NUI's offsets inside the window, measured from screenshots (hit tests) */
#define HIT_ROWS -10.0f
#define ROWS 6       /* reply rows when they scroll (4 replies + "..." rows) */
#define ROW_GAP 4.0f /* NUI's spacing between rows */
#define QUICKBAR_H 60.0f /* keep clear of the quickbar at the bottom */
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
    bool preview;         /* debug: a made-up conversation is shown */
    int flip;             /* which of the two tokens is current */
    int first, last;      /* the replies shown (more than fit on screen scroll) */
    float rh[NWPAD_DIALOG_REPLIES]; /* each reply's row height */
    float room;           /* height available for the replies */
    int nrows, row_kind[ROWS + 2]; /* the rows: a reply index, or -1 for "..." */
    float row_h[ROWS + 2];
    int bound_kind[ROWS + 2], bound_colour[ROWS + 2]; /* what each row's binds hold (-2 nothing) */
    float win_h, text_y0, text_y1, rows_y0; /* the layout, GUI units from the window's top (hit tests) */
    char wrapped[4200];   /* the NPC's line, wrapped by nwpad: lines separated by '\n' */
    int line_at[512], lines, top, visible; /* line starts in wrapped; the shown range */
    uint32_t seq;
    int highlight;
    nwpad_dialog d;
    float nav_dir;        /* the stick's held direction (-1, 0, +1) */
    uint64_t nav_next_ms; /* when a held stick steps again */
} ui = {.highlight = -1};

bool nwpad_dialogui_open(void) { return ui.open; }
int nwpad_dialogui_highlight(void) { return ui.open ? ui.highlight : -1; }
int nwpad_dialogui_text_top(void) { return ui.open ? ui.top : -1; }
void nwpad_dialogui_range(int *first, int *last) {
    *first = ui.open ? ui.first : -1;
    *last = ui.open ? ui.last : -1;
}

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

/* Game text -> JSON string body, through the game's UTF-8 conversion. */
static void game_json(char *out, size_t cap, const char *text) {
    static char utf8[12000];
    nwpad_text_utf8(text, utf8, sizeof utf8);
    nwpad_utf8_fold_punctuation(utf8);
    nwpad_json_escape_utf8(out, cap, utf8);
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
    game_json(esc, sizeof esc, text);
    snprintf(value, sizeof value, "\"%s\"", esc);
    nwpad_nui_bind(TOKEN, "body", value);
    show_thumb();
}

static void reply_text(int i, char *out, size_t cap) {
    char numbered[600];
    snprintf(numbered, sizeof numbered, "%d. %s", i + 1, ui.d.replies[i].text);
    game_json(out, cap, numbered);
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

/* Screen height in GUI units (as the picker centres itself). */
static float screen_h(void) {
    float (*scale)(void) = (float (*)(void))nwpad_sig(NWPAD_SIG_GUI_SCALE);
    void **gui = (void **)nwpad_sig(NWPAD_SIG_GUI_MANAGER);
    float s = scale ? scale() : 1.0f;
    int h = gui && *gui ? *(int *)((char *)*gui + 0xbc) : 720;
    return (float)h / (s > 0 ? s : 1.0f);
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
    game_json(name, sizeof name, ui.d.speaker_name);
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
        "{\"type\":\"text\",\"label\":null,\"value\":{\"bind\":\"body\"},\"border\":false,\"scrollbars\":0,"
        "\"height\":%.1f%s}%s]}]}]},"
        /* a subtle line between the NPC's text and the replies */
        "{\"type\":\"spacer\",\"label\":null,\"value\":null,\"height\":%.1f,\"draw_list_scissor\":false,"
        "\"draw_list\":[{\"type\":6,\"enabled\":true,\"color\":{\"r\":173,\"g\":142,\"b\":96,\"a\":110},"
        "\"fill\":null,\"line_thickness\":1.0,\"order\":1,\"render\":0,\"arrayBinds\":false,"
        "\"a\":{\"x\":0.0,\"y\":%.1f},\"b\":{\"x\":%.1f,\"y\":%.1f}}]}",
        ui.d.portrait, PORTRAIT_W, PORTRAIT_H, name, LINE_H, body_h, big_font ? ",\"font\":\"" FONT "\"" : "",
        scrolls ? scrollbar(body_h - BAR_TRIM) : "", SEP_H, SEP_H / 2, WIDTH - 2 * PAD, SEP_H / 2);
    ui.text_y0 = HIT_TOP + LINE_H;           /* the NPC's text, under the name */
    ui.text_y1 = ui.text_y0 + body_h;
    ui.rows_y0 = h + HIT_ROWS;               /* the first reply row */
    ui.room = screen_h() - h - 2 * PAD - QUICKBAR_H;
    for (int i = 0; i < ui.d.count; i++) {
        char numbered[600];
        snprintf(numbered, sizeof numbered, "%d. %s", i + 1, ui.d.replies[i].text);
        ui.rh[i] = text_height(numbered, WIDTH - 2 * PAD) + ROW_GAP;
    }
    layout();
    for (int k = 0; k < ui.nrows && n < sizeof json; k++) {
        h += ui.row_h[k];
        n += (size_t)snprintf(json + n, sizeof json - n,
            ",{\"type\":\"text\",\"label\":null,\"value\":{\"bind\":\"t%d\"},\"border\":false,\"scrollbars\":0,"
            "\"height\":%.1f,\"foreground_color\":{\"bind\":\"c%d\"}}", k, ui.row_h[k] - ROW_GAP, k);
    }
    if (ui.d.panel_h + 4 > h) h = ui.d.panel_h + 4; /* never smaller than the game's window */
    ui.win_h = h;
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
    snprintf(geo, sizeof geo, "{\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f}", WIN_X, WIN_Y, WIDTH, h);
    nwpad_nui_bind(TOKEN, "geo", geo);
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

/* Show the window again after the highlight left the visible replies. */
unsigned nwpad_dialogui_builds; uint64_t nwpad_dialogui_build_ns, nwpad_dialogui_build_max_ns; /* debug */
static void rebuild(void) {
    uint64_t t0 = nwpad_now_ns(), g0 = nwpad_nui_take_game_ns();
    nwpad_nui_destroy(TOKEN);
    ui.flip ^= 1;
    ui.open = build();
    uint64_t game = nwpad_nui_take_game_ns(), own = nwpad_now_ns() - t0 - game;
    nwpad_game_ns_add(g0 + game); /* put the game time back for the frame's account */
    nwpad_dialogui_builds++;
    nwpad_dialogui_build_ns += own;
    if (own > nwpad_dialogui_build_max_ns) nwpad_dialogui_build_max_ns = own;
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
    if (!same) { /* other row heights (a long reply came into view): build again */
        rebuild();
        return;
    }
    for (int k = 0; k < ui.nrows; k++) bind_row(k); /* scrolled or not: just the binds */
}

/* GUI units per window pixel (NUI geometry is in GUI units). */
static float gui_scale(void) {
    float (*scale)(void) = (float (*)(void))nwpad_sig(NWPAD_SIG_GUI_SCALE);
    float s = scale ? scale() : 1.0f;
    return s > 0 ? s : 1.0f;
}

int nwpad_dialogui_hit(int px, int py) {
    if (!ui.open) return NWPAD_DLG_OUTSIDE;
    float s = gui_scale(), x = (float)px / s - WIN_X, y = (float)py / s - WIN_Y;
    if (x < 0 || y < 0 || x >= WIDTH || y >= ui.win_h) return NWPAD_DLG_OUTSIDE;
    if (y >= ui.text_y0 && y < ui.text_y1 && x >= PORTRAIT_W) return NWPAD_DLG_TEXT;
    float top = ui.rows_y0;
    for (int k = 0; k < ui.nrows; k++) {
        if (y >= top && y < top + ui.row_h[k]) return k;
        top += ui.row_h[k];
    }
    return NWPAD_DLG_OTHER;
}

void nwpad_dialogui_hover(int row) {
    if (!ui.open || row < 0 || row >= ui.nrows) return;
    int i = ui.row_kind[row];
    if (i < 0 || i == ui.highlight || !ui.d.replies[i].selectable) return;
    ui.highlight = i; /* it's on screen: only colours change */
    for (int k = 0; k < ui.nrows; k++) bind_row(k);
}

void nwpad_dialogui_click(int row) {
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

void nwpad_dialogui_wheel(int hit, int notches) {
    if (!ui.open || !notches) return;
    if (hit == NWPAD_DLG_TEXT) nwpad_dialogui_scroll(-3 * notches); /* wheel up: earlier lines */
    else nwpad_dialogui_move(notches > 0 ? -1 : 1);
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
    if (ui.open) nwpad_nui_destroy(TOKEN);
    ui.open = ui.preview = false;
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
    ui.flip ^= 1;
    ui.highlight = first_selectable();
    ui.first = ui.top = 0;
    ui.open = ui.preview = build();
    return ui.open;
}
#endif

bool nwpad_dialogui_frame(bool enabled, float nav, uint64_t now_ms) {
    if (ui.preview) return true;
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
        nwpad_strip_colour_codes(ui.d.line); /* no inline colours in NUI text */
        trim(ui.d.line);
        for (int i = 0; i < ui.d.count; i++) {
            nwpad_strip_colour_codes(ui.d.replies[i].text);
            trim(ui.d.replies[i].text);
        }
        ui.first = 0;
        ui.top = 0;
        if (ui.open) {
            nwpad_nui_destroy(TOKEN);
            ui.flip ^= 1;
        }
        ui.highlight = first_selectable();
        ui.open = build();
        if (!ui.open) return false;
    }
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
