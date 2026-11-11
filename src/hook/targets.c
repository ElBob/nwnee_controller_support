/* See targets.h. The area's object list is the one the game's highlight-all walks
 * (CNWCArea+0x318, count +0x320, re-notes F40); a target is an object with +0xa8 and +0x11c
 * set (the game's "useable" flag) of a type the click handles. */
#include "targets.h"

#include "backend.h"
#include "game.h"
#include "sigs.h"
#include "../core/nwpad_core.h"

#include <math.h>
#include <string.h>

enum { OBJ_TYPE = 0xc, OBJ_ID = 0x8, OBJ_POS = 0x38, OBJ_FLAG_A8 = 0xa8, OBJ_USEABLE = 0x11c };
enum { KIND_CREATURE = 5, KIND_ITEM = 6, KIND_PLACEABLE = 9, KIND_DOOR = 10 };
#define MAX_TARGETS 256

typedef struct { float x, y, z; } vec3; /* the game's Vector, passed in two SSE registers */

static struct {
    bool active;
    uint32_t selected;
    int count;
} tg;

bool nwpad_targets_active(void) { return tg.active; }

#ifdef NWPAD_DEBUG_SURFACES
uint32_t nwpad_targets_selected(void) { return tg.active ? tg.selected : 0; }
int nwpad_targets_count(void) { return tg.count; }
#endif

/* The game's AsNWCObject for an id, or NULL. */
static char *object_of(uint32_t id) {
    void *(*get)(void *, uint32_t) = (void *(*)(void *, uint32_t))nwpad_sig(NWPAD_SIG_CLIENT_GET_GAME_OBJECT);
    void *app = nwpad_client_app();
    void **g = get && app ? get(app, id) : NULL;
    return g ? (char *)((void **(*)(void *))((void **)*g)[0x20 / 8])(g) : NULL;
}

/* Every selectable object of the client's area. */
static int collect(nwpad_target *out, int cap) {
    char *area = nwpad_client_area();
    int total = area ? *(int *)(area + 800) : 0, n = 0;
    const uint32_t *ids = area ? *(uint32_t **)(area + 0x318) : NULL;
    for (int i = 0; ids && i < total && n < cap; i++) {
        char *o = object_of(ids[i]);
        if (!o || !(uint8_t)o[OBJ_FLAG_A8] || *(int *)(o + OBJ_USEABLE) == 0) continue;
        int kind = (uint8_t)o[OBJ_TYPE];
        if (kind != KIND_CREATURE && kind != KIND_ITEM && kind != KIND_PLACEABLE && kind != KIND_DOOR) continue;
        out[n].x = *(float *)(o + OBJ_POS);
        out[n].y = *(float *)(o + OBJ_POS + 4);
        out[n].id = ids[i];
        out[n].kind = kind;
        n++;
    }
    return n;
}

/* Light (on) or un-light the object the way the game's highlight-all does: the object's own
 * Hilite (vslot 0x160) and UnHilite (0x168), plus a creature's name plate (MouseOver 0x110 /
 * MouseOff 0x118). */
static void light(uint32_t id, bool on) {
    char *o = object_of(id);
    if (!o) return;
    void **vt = *(void ***)o;
    int kind = (uint8_t)o[OBJ_TYPE];
    if (on) {
        if (kind == KIND_CREATURE) ((void (*)(void *, int))vt[0x110 / 8])(o, 1);
        /* orange suits a creature; on the brown barrels and chests around it is nearly
         * invisible, so everything else is lit green */
        vec3 colour = kind == KIND_CREATURE ? (vec3){1.0f, 0.82f, 0.25f} : (vec3){0.2f, 1.0f, 0.3f};
        ((void (*)(void *, vec3, uint8_t, float, int))vt[0x160 / 8])(o, colour, 2, 1.0f, 1);
    } else {
        if (kind == KIND_CREATURE) ((void (*)(void *))vt[0x118 / 8])(o);
        ((void (*)(void *, uint8_t))vt[0x168 / 8])(o, 2);
    }
}

bool nwpad_targets_step(int dir, float camera_forward_deg) {
    float px, py;
    if (!nwpad_backend_player_pos_world(&px, &py)) return false;
    nwpad_target all[MAX_TARGETS], list[MAX_TARGETS];
    int n = collect(all, MAX_TARGETS);
    n = nwpad_targets_sort(all, n, px, py, camera_forward_deg, NWPAD_TARGET_RANGE, NWPAD_TARGET_CONE, list, MAX_TARGETS);
    tg.count = n;
    int next = nwpad_target_step(list, n, tg.active ? tg.selected : 0, dir);
    if (next < 0) return false;
    if (tg.active && tg.selected) light(tg.selected, false);
    tg.selected = list[next].id;
    tg.active = true;
    light(tg.selected, true);
    return true;
}

void nwpad_targets_end(void) {
    if (!tg.active) return;
    if (tg.selected) light(tg.selected, false);
    tg.active = false;
    tg.selected = 0;
}

void nwpad_targets_frame(void) {
    if (!tg.active) return;
    char *o = tg.selected ? object_of(tg.selected) : NULL;
    if (!o || !nwpad_backend_in_game()) { /* gone, or we left the game: nothing to un-light */
        tg.active = false;
        tg.selected = 0;
    }
}
