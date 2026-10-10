/* See game.h. */
#include "game.h"

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

void nwpad_gui_size(float *w, float *h) {
    float (*scale)(void) = (float (*)(void))nwpad_sig(NWPAD_SIG_GUI_SCALE);
    void **gui_var = (void **)nwpad_sig(NWPAD_SIG_GUI_MANAGER);
    char *gui = gui_var ? (char *)*gui_var : NULL;
    float s = scale ? scale() : 1.0f;
    if (s <= 0) s = 1.0f;
    *w = (gui ? (float)*(int32_t *)(gui + GUI_WIDTH) : 1280.0f * s) / s;
    *h = (gui ? (float)*(int32_t *)(gui + GUI_WIDTH + 4) : 720.0f * s) / s;
}
