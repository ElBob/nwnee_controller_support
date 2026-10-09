/* See icons.h. How the engine colours PLTs (F35): a PLT pixel is (shade, layer);
 * the item gives each of the 10 layers a colour value v; the engine's stacked palette
 * holds the seven pal_* textures at 256 rows each, so the pixel is
 * palette[v >> 8] row (v & 0xff), column shade. Shade 255 is transparent (alpha 0).
 * PLT rows, like TGA rows, run bottom to top. */
#include "icons.h"

#include "../core/nwpad_core.h"
#include "settings.h"
#include "sigs.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { char *ptr; uint32_t len; } exo_string;
typedef struct { void *ptr; void *ctrl; } shared_ptr; /* std::shared_ptr */
typedef void (*res_get_fn)(shared_ptr *out, void *resman, const void *resref, unsigned short type);
typedef int (*add_key_table_fn)(void *resman, unsigned id, const exo_string *name, unsigned type, int dynamic,
                                void *filter);
typedef void (*release_fn)(void *ctrl);

enum { RES_TGA = 3, RES_PLT = 6, PALETTES = 7, PAL_W = 256 };
/* The rendered icons live in a subdirectory of the game's TEMPCLIENT directory
 * (<userdir>/tempclient), registered as nwpad's own key table (F35). */
#define ICON_TABLE "TEMPCLIENT:nwpadicons"
#define ICON_TABLE_ID 98500000u
static const char *const palette_names[PALETTES] = {"pal_skin01",  "pal_hair01",  "pal_armor01", "pal_armor02",
                                                    "pal_cloth01", "pal_leath01", "pal_tattoo01"};

/* Raw resource bytes (malloc'd copy) through the game's resource manager, so haks
 * and override folders apply as in the game. */
uint8_t *nwpad_resource_get(const char *name, unsigned short type, size_t *size) {
    res_get_fn get = (res_get_fn)nwpad_sig(NWPAD_SIG_RES_GET);
    void **resman = (void **)nwpad_sig(NWPAD_SIG_RES_MAN);
    release_fn release = (release_fn)nwpad_sig(NWPAD_SIG_SHARED_RELEASE);
    if (!get || !resman || !*resman || !release) return NULL;
    char ref[32] = {0}; /* CResRef: 16 characters */
    for (int i = 0; i < 16 && name[i]; i++) ref[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    shared_ptr block = {0};
    get(&block, *resman, ref, type);
    uint8_t *copy = NULL;
    /* shared_ptr<DataBlock>; DataBlock holds a shared_ptr to {data, size, ...} (F35) */
    void *inner = block.ptr ? ((void **)block.ptr)[0] : NULL;
    if (inner) {
        uint8_t *data = ((uint8_t **)inner)[0];
        size_t n = ((size_t *)inner)[1];
        if (data && n && (copy = malloc(n))) {
            memcpy(copy, data, n);
            *size = n;
        }
    }
    if (block.ctrl) release(block.ctrl);
    return copy;
}

/* A TGA (uncompressed or RLE, 24/32 bit) as RGBA rows top to bottom. */
static uint8_t *decode_tga(const uint8_t *t, size_t n, int *w, int *h) {
    if (n < 18) return NULL;
    int type = t[2], bpp = t[16], top = t[17] & 0x20;
    *w = t[12] | t[13] << 8;
    *h = t[14] | t[15] << 8;
    if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32) || *w <= 0 || *h <= 0) return NULL;
    size_t px = (size_t)*w * (size_t)*h, bytes = (size_t)bpp / 8, at = 18 + t[0] + (size_t)(t[1] ? (t[5] | t[6] << 8) * ((t[7] + 7) / 8) : 0);
    uint8_t *rgba = calloc(px, 4);
    if (!rgba) return NULL;
    for (size_t i = 0; i < px;) {
        size_t run = 1;
        bool repeat = false;
        if (type == 10) {
            if (at >= n) break;
            repeat = t[at] & 0x80;
            run = (size_t)(t[at++] & 0x7f) + 1;
        }
        for (size_t k = 0; k < run && i < px; k++, i++) {
            if (at + bytes > n) { free(rgba); return NULL; }
            size_t y = i / (size_t)*w, x = i % (size_t)*w, row = top ? y : (size_t)*h - 1 - y;
            uint8_t *o = rgba + (row * (size_t)*w + x) * 4;
            o[0] = t[at + 2]; o[1] = t[at + 1]; o[2] = t[at]; o[3] = bytes == 4 ? t[at + 3] : 255;
            if (!repeat || k + 1 == run) at += bytes;
        }
    }
    return rgba;
}

static struct { uint8_t *rgba; int rows; bool tried; } palette[PALETTES];

static const uint8_t *palette_px(int pal, int row, int shade) {
    if (pal < 0 || pal >= PALETTES) return NULL;
    if (!palette[pal].tried) {
        palette[pal].tried = true;
        size_t n = 0;
        uint8_t *t = nwpad_resource_get(palette_names[pal], RES_TGA, &n);
        int w = 0, h = 0;
        palette[pal].rgba = t ? decode_tga(t, n, &w, &h) : NULL;
        free(t);
        if (palette[pal].rgba && w != PAL_W) { free(palette[pal].rgba); palette[pal].rgba = NULL; }
        palette[pal].rows = h;
    }
    if (!palette[pal].rgba || row >= palette[pal].rows) return NULL;
    return palette[pal].rgba + ((size_t)row * PAL_W + (size_t)shade) * 4;
}

/* Paint PLT `name` with layer colours over *canvas (alpha over); the first part
 * sizes the canvas, later ones must match it. */
static bool paint_plt(uint8_t **canvas, int *cw, int *ch, const char *name, const uint16_t *colours, int count) {
    size_t n = 0;
    uint8_t *p = nwpad_resource_get(name, RES_PLT, &n);
    if (!p) return false;
    bool ok = false;
    if (n >= 24 && !memcmp(p, "PLT V1  ", 8)) {
        uint32_t w, h;
        memcpy(&w, p + 16, 4);
        memcpy(&h, p + 20, 4);
        if (!*canvas && w > 0 && h > 0 && w <= 512 && h <= 512 && (*canvas = calloc((size_t)w * h, 4))) {
            *cw = (int)w;
            *ch = (int)h;
        }
        if (*canvas && w == (uint32_t)*cw && h == (uint32_t)*ch && n >= 24 + (size_t)w * h * 2) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                int shade = p[24 + i * 2], layer = p[24 + i * 2 + 1];
                uint16_t v = layer < count ? colours[layer] : 0;
                const uint8_t *c = palette_px(v >> 8, v & 0xff, shade);
                if (!c || !c[3]) continue;
                size_t y = i / w, x = i % w; /* PLT rows run bottom to top, like TGA */
                uint8_t *o = *canvas + ((h - 1 - y) * w + x) * 4;
                unsigned a = c[3], ia = 255 - a;
                for (int k = 0; k < 3; k++) o[k] = (uint8_t)((c[k] * a + o[k] * ia) / 255);
                o[3] = (uint8_t)(a + o[3] * ia / 255);
            }
            ok = true;
        }
    }
    free(p);
    return ok;
}

static const char *icon_dir(void) {
    static char dir[512];
    if (!dir[0]) {
        char user[400];
        nwpad_user_directory(user, sizeof user);
        snprintf(dir, sizeof dir, "%s/tempclient/nwpadicons/", user);
    }
    char parent[520];
    snprintf(parent, sizeof parent, "%.*s", (int)(strlen(dir) - strlen("nwpadicons/")), dir);
    mkdir(parent, 0755);
    mkdir(dir, 0755); /* the game may clear tempclient */
    return dir;
}

/* Register the icon directory, or rescan it (adding a key table of the same name
 * again rebuilds it, F35), so the resource manager sees new files. */
static bool registered; /* this session (files may be left from an earlier one) */

static bool register_dir(void) {
    add_key_table_fn add = (add_key_table_fn)nwpad_sig(NWPAD_SIG_RES_ADD_KEY_TABLE);
    void **resman = (void **)nwpad_sig(NWPAD_SIG_RES_MAN);
    if (!add || !resman || !*resman) return false;
    exo_string table = {(char *)ICON_TABLE, (uint32_t)strlen(ICON_TABLE)}; /* length without the NUL */
    registered = add(*resman, ICON_TABLE_ID, &table, 2 /* directory */, 1, NULL) != 0;
    return registered;
}

bool nwpad_resource_publish(const char *filename, const void *data, size_t size) {
    char path[640], tmp[660];
    snprintf(path, sizeof path, "%s%s", icon_dir(), filename);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = rename(tmp, path) == 0;
    else unlink(tmp);
    return ok && register_dir();
}

static bool write_tga(const char *path, const uint8_t *rgba, int w, int h) {
    char tmp[640];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    uint8_t head[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), 32, 0x28};
    bool ok = fwrite(head, 1, sizeof head, f) == sizeof head;
    for (size_t i = 0; ok && i < (size_t)w * (size_t)h; i++) {
        uint8_t bgra[4] = {rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4], rgba[i * 4 + 3]};
        ok = fwrite(bgra, 1, 4, f) == 4;
    }
    if (fclose(f) != 0) ok = false;
    if (ok) ok = rename(tmp, path) == 0;
    else unlink(tmp);
    return ok;
}

bool nwpad_icon_render_plt(const char *icon, char kind, char out[17]) {
    /* The layers and their colours, as CLayeredIcon / CArmorIcon keep them (F34). */
    const char *parts[6];
    const uint16_t *colours[6];
    int count, n = 0;
    if (kind == 'L') {
        count = *(const int *)(icon + 0x48);
        parts[n] = icon + 0x08;
        colours[n++] = *(uint16_t *const *)(icon + 0x50);
    } else {
        count = *(const int *)(icon + 0xb0);
        const uint16_t *base = *(uint16_t *const *)(icon + 0xb8), *per_part = *(uint16_t *const *)(icon + 0xc0);
        for (int k = 0; k < 6; k++) {
            const char *ref = icon + 0x48 + 0x11 * k;
            if (!ref[0]) continue;
            parts[n] = ref;
            colours[n++] = per_part ? per_part + (size_t)count * (size_t)k : base;
        }
    }
    if (n == 0 || count <= 0 || count > 16) return false;
    /* Name from what the image depends on: parts and colours. */
    uint64_t hash = 1469598103934665603ull;
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < 16 && parts[k][i]; i++) hash = (hash ^ (uint8_t)(parts[k][i] | 0x20)) * 1099511628211ull;
        for (int i = 0; colours[k] && i < count; i++) hash = (hash ^ colours[k][i]) * 1099511628211ull;
    }
    snprintf(out, 17, "nwpd%012llx", (unsigned long long)(hash & 0xffffffffffffull));
    char path[600];
    snprintf(path, sizeof path, "%s%s.tga", icon_dir(), out);
    if (access(path, F_OK) == 0) return registered || register_dir(); /* rendered before */
    int w = 0, h = 0;
    uint8_t *canvas = NULL;
    bool painted = false;
    char ref[17];
    for (int k = 0; k < n; k++) {
        memcpy(ref, parts[k], 16);
        ref[16] = '\0';
        if (colours[k] && paint_plt(&canvas, &w, &h, ref, colours[k], count)) painted = true;
    }
    bool ok = painted && write_tga(path, canvas, w, h);
    free(canvas);
    return ok && register_dir();
}


#ifdef NWPAD_DEBUG_SURFACES
size_t nwpad_icon_debug_fetch(const char *name) {
    size_t n = 0;
    free(nwpad_resource_get(name, RES_TGA, &n));
    return n;
}
#endif
