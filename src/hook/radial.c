/* See radial.h. The open menus are the game's modal panels (CGuiMan+0x88, count +0x90); the
 * top one is the one a click would reach. The game picks an entry from where the pointer
 * is around the menu's centre (+0x85c / +0x860): HandleMouseMoveCaptured, so the stick
 * is turned into a pointer position that far from the centre. State +0x848: 2 an entry is
 * picked, 3 a sub menu entry, 0/1 the centre, 5 outside (re-notes F41). */
#include "radial.h"

#include <math.h>
#include <string.h>

#include "game.h"
#include "sigs.h"

enum { GUI_MODALS = 0x88, GUI_MODAL_COUNT = 0x90, GUI_MOUSE_X = 0xd4, GUI_MOUSE_Y = 0xd8, GUI_WIDTH = 0xb8 };
enum { MENU_CENTER_X = 0x85c, MENU_CENTER_Y = 0x860, MENU_STATE = 0x848, MENU_PICKED = 0x84c };
enum { MENU_PARENT = 0x358 };
enum { OBJ_VSLOT_RADIAL = 0x150, INTERNAL_RADIAL_TARGET = 0x424, KEY_ESCAPE = 0x12 };
#define PICK_RADIUS 100.0f /* GUI units: the game's entry ring is 56..150 */

static char *gui(void) {
    void **v = (void **)nwpad_sig(NWPAD_SIG_GUI_MANAGER);
    return v ? (char *)*v : NULL;
}

bool nwpad_radial_open(void) {
    int *depth = (int *)nwpad_sig(NWPAD_SIG_RADIAL_MENU_DEPTH);
    return depth && *depth > 0;
}

/* The menu a click would reach: the top modal panel. */
static char *top_menu(void) {
    char *g = gui();
    if (!g || !nwpad_radial_open() || *(int *)(g + GUI_MODAL_COUNT) <= 0) return NULL;
    return (*(char ***)(g + GUI_MODALS))[*(int *)(g + GUI_MODAL_COUNT) - 1];
}

bool nwpad_radial_open_for(uint32_t id) {
    void *(*object)(void *, uint32_t) = (void *(*)(void *, uint32_t))nwpad_sig(NWPAD_SIG_CLIENT_GET_GAME_OBJECT);
    void *app = nwpad_client_app();
    char *internal = app ? *(char **)((char *)app + 0x8) : NULL;
    char *g = gui();
    void **go = object && internal && g ? object(app, id) : NULL;
    void **nwc = go ? ((void **(*)(void *))((void **)*go)[0x20 / 8])(go) : NULL; /* AsNWCObject */
    if (!nwc || nwpad_radial_open()) return false;
    /* The menu opens where the game thinks the pointer is: the middle of the screen. */
    *(int32_t *)(g + GUI_MOUSE_X) = *(int32_t *)(g + GUI_WIDTH) / 2;
    *(int32_t *)(g + GUI_MOUSE_Y) = *(int32_t *)(g + GUI_WIDTH + 4) / 2;
    *(uint32_t *)(internal + INTERNAL_RADIAL_TARGET) = id; /* as the right click records it */
    ((void (*)(void *))((void **)*nwc)[OBJ_VSLOT_RADIAL / 8])(nwc);
    return nwpad_radial_open();
}

void nwpad_radial_frame(nwpad_vec2 stick) {
    char *m = top_menu();
    void (*move)(void *, int, int) = (void (*)(void *, int, int))nwpad_sig(NWPAD_SIG_RADIAL_MENU_MOVE);
    float (*scale)(void) = (float (*)(void))nwpad_sig(NWPAD_SIG_GUI_SCALE);
    if (!m || !move || nwpad_magnitude(stick) < NWPAD_PICKER_MIN) return;
    /* Quantized: the pointer goes to the exact direction of the nearest of the eight entries
     * (they sit at multiples of 45 degrees from up), so the stick only ever picks an entry.
     * The GUI's y runs up, like the stick's (F41). */
    float s = scale ? scale() : 1.0f;
    float rad = (float)lround(nwpad_stick_angle_cw(stick) / 45.0f) * 45.0f * NWPAD_RAD_PER_DEG;
    int x = *(int *)(m + MENU_CENTER_X) + (int)lroundf(sinf(rad) * PICK_RADIUS * s);
    int y = *(int *)(m + MENU_CENTER_Y) + (int)lroundf(cosf(rad) * PICK_RADIUS * s);
    move(m, x, y);
}

void nwpad_radial_confirm(void) {
    char *m = top_menu();
    void (*act)(void *) = (void (*)(void *))nwpad_sig(NWPAD_SIG_RADIAL_MENU_ACTION);
    int state = m ? *(int *)(m + MENU_STATE) : 0;
    if (act && (state == 2 || state == 3)) act(m);
}

/* B: up one level, as the centre of a sub menu does (the game re-opens the parent when the
 * pointer is in the centre and it is acted on, F41); from the top menu, close it (Escape). */
void nwpad_radial_cancel(void) {
    char *m = top_menu();
    void (*move)(void *, int, int) = (void (*)(void *, int, int))nwpad_sig(NWPAD_SIG_RADIAL_MENU_MOVE);
    void (*act)(void *) = (void (*)(void *))nwpad_sig(NWPAD_SIG_RADIAL_MENU_ACTION);
    unsigned (*key)(void *, unsigned, unsigned) = (unsigned (*)(void *, unsigned, unsigned))nwpad_sig(NWPAD_SIG_RADIAL_MENU_KEY);
    if (!m) return;
    if (*(void **)(m + MENU_PARENT) && move && act) {
        move(m, *(int *)(m + MENU_CENTER_X), *(int *)(m + MENU_CENTER_Y));
        act(m);
    } else if (key) {
        key(m, KEY_ESCAPE, 1);
    }
}

#ifdef NWPAD_DEBUG_SURFACES
int nwpad_radial_depth(void) {
    int *depth = (int *)nwpad_sig(NWPAD_SIG_RADIAL_MENU_DEPTH);
    return depth ? *depth : -1;
}

int nwpad_radial_level(void) { /* 0 the top menu, 1 a sub menu, ...; -1 none open */
    char *m = top_menu();
    int n = -1;
    for (; m; m = *(char **)(m + MENU_PARENT)) n++;
    return n;
}

int nwpad_radial_picked(void) {
    char *m = top_menu();
    int state = m ? *(int *)(m + MENU_STATE) : 0;
    return m && (state == 2 || state == 3) ? *(int *)(m + MENU_PICKED) : -1;
}
#endif
