/* See game.h. */
#include "game.h"

#include <stdbool.h>

#include "sigs.h"

#define GUI_WIDTH 0xb8 /* int, on CGuiMan (height at +0xbc; re-notes F26) */

void *nwpad_client_app(void) {
    void **mgr_var = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    void **mgr = mgr_var ? (void **)*mgr_var : NULL;
    return mgr ? *mgr : NULL;
}

void *nwpad_in_game_gui(void) {
    void *(*get_gui)(void *) = (void *(*)(void *))nwpad_sig(NWPAD_SIG_CLIENT_GET_IN_GAME_GUI);
    void *app = get_gui ? nwpad_client_app() : NULL;
    return app ? get_gui(app) : NULL;
}

char *nwpad_client_area(void) {
    char *app = nwpad_client_app();
    char *internal = app ? *(char **)(app + 0x8) : NULL;
    char *module = internal ? *(char **)(internal + 0x30) : NULL;
    return module ? *(char **)(module + 0xf8) : NULL;
}

#ifdef NWPAD_DEBUG_SURFACES
#include <stdio.h>
#include <string.h>

void nwpad_game_debug_area_objects(char *out, size_t cap, int max) {
    char *area = nwpad_client_area();
    void *(*object)(void *, uint32_t) = (void *(*)(void *, uint32_t))nwpad_sig(NWPAD_SIG_CLIENT_GET_GAME_OBJECT);
    void *app = nwpad_client_app();
    if (!area || !object || !app) {
        snprintf(out, cap, "{\"error\":\"no area (%d) or lookup\"}", area != NULL);
        return;
    }
    int count = *(int *)(area + 800);
    uint32_t *ids = *(uint32_t **)(area + 0x318);
    size_t n = (size_t)snprintf(out, cap, "{\"count\":%d,\"objects\":[", count);
    int shown = 0;
    for (int i = 0; ids && i < count && shown < max && n + 200 < cap; i++) {
        void **g = object(app, ids[i]);
        void **nwc = g ? ((void **(*)(void *))((void **)*g)[0x20 / 8])(g) : NULL; /* AsNWCObject */
        if (!nwc) continue;
        char *o = (char *)nwc;
        n += (size_t)snprintf(out + n, cap - n,
                              "%s{\"id\":\"%08x\",\"type\":%u,\"a8\":%u,\"f11c\":%d,\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,"
                              "\"flags120\":%u,\"anim\":\"%016llx\",\"f10c\":%.2f}",
                              shown ? "," : "", ids[i], (unsigned)(uint8_t)o[0xc], (unsigned)(uint8_t)o[0xa8],
                              *(int *)(o + 0x11c), *(float *)(o + 0x38), *(float *)(o + 0x3c), *(float *)(o + 0x40),
                              (unsigned)(*(uint8_t *)(o + 0x120)), (unsigned long long)*(uint64_t *)(o + 0x78),
                              *(float *)(o + 0x10c));
        shown++;
    }
    if (n < cap) snprintf(out + n, cap - n, "],\"shown\":%d}", shown);
}

typedef struct { float x, y, z; } vec3; /* passed by value in two SSE registers, as the game's Vector */

int nwpad_game_debug_hilite(uint32_t id, bool on, float r, float g, float b) {
    void *(*object)(void *, uint32_t) = (void *(*)(void *, uint32_t))nwpad_sig(NWPAD_SIG_CLIENT_GET_GAME_OBJECT);
    void *app = nwpad_client_app();
    void **gobj = object && app ? object(app, id) : NULL;
    void **nwc = gobj ? ((void **(*)(void *))((void **)*gobj)[0x20 / 8])(gobj) : NULL; /* AsNWCObject */
    if (!nwc) return -1;
    void **vt = (void **)*nwc;
    int type = ((uint8_t *)nwc)[0xc];
    if (on) {
        if (type == 5) ((void (*)(void *, int))vt[0x110 / 8])(nwc, 1); /* creature MouseOver (its name) */
        ((void (*)(void *, vec3, uint8_t, float, int))vt[0x160 / 8])(nwc, (vec3){r, g, b}, 2, 1.0f, 1);
    } else {
        if (type == 5) ((void (*)(void *))vt[0x118 / 8])(nwc); /* MouseOff */
        ((void (*)(void *, uint8_t))vt[0x168 / 8])(nwc, 2);
    }
    return type;
}

int nwpad_game_debug_click_object(uint32_t id) {
    void *(*object)(void *, uint32_t) = (void *(*)(void *, uint32_t))nwpad_sig(NWPAD_SIG_CLIENT_GET_GAME_OBJECT);
    uint64_t (*click)(void *, void *) = (uint64_t (*)(void *, void *))nwpad_sig(NWPAD_SIG_CLIENT_DEFAULT_ACTION_ON_OBJECT);
    void *app = nwpad_client_app();
    char *internal = app ? *(char **)((char *)app + 0x8) : NULL;
    void **g = object && internal ? object(app, id) : NULL;
    void **nwc = g ? ((void **(*)(void *))((void **)*g)[0x20 / 8])(g) : NULL; /* AsNWCObject */
    if (!nwc || !click) return -1;
    return (int)click(internal, nwc);
}

bool nwpad_game_menu_open(void) {
    char *gui = nwpad_in_game_gui();
    return gui && *(void **)(gui + 0xd8) != NULL;
}

bool nwpad_game_map_open(void) {
    char *gui = nwpad_in_game_gui();
    char *panel = gui ? *(char **)(gui + 0xb8) : NULL;
    return panel && *(int *)(panel + 0xb8) == 1; /* the "active" state ToggleMap tests before it closes */
}
#endif

void nwpad_gui_size(float *w, float *h) {
    float (*scale)(void) = (float (*)(void))nwpad_sig(NWPAD_SIG_GUI_SCALE);
    void **gui_var = (void **)nwpad_sig(NWPAD_SIG_GUI_MANAGER);
    char *gui = gui_var ? (char *)*gui_var : NULL;
    float s = scale ? scale() : 1.0f;
    if (s <= 0) s = 1.0f;
    *w = (gui ? (float)*(int32_t *)(gui + GUI_WIDTH) : 1280.0f * s) / s;
    *h = (gui ? (float)*(int32_t *)(gui + GUI_WIDTH + 4) : 720.0f * s) / s;
}
