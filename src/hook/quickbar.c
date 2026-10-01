/* See quickbar.h. Layouts from re-notes F31. */
#include "quickbar.h"

#include "sigs.h"

#include <stdio.h>
#include <string.h>

typedef struct { char *ptr; uint32_t len; } exo_string; /* CExoString */

typedef void *(*vcall_fn)(void *self);
typedef void *(*get_spell_fn)(void *spell_array, int id);
typedef void (*string_of_fn)(exo_string *out, void *self);             /* GetSpellNameText */
typedef void (*tlk_string_fn)(exo_string *out, void *tlk, unsigned strref);
typedef void *(*item_by_id_fn)(void *client_app, uint32_t oid);
typedef void (*item_name_fn)(exo_string *out, void *item, int full);
typedef void (*exo_string_dtor_fn)(exo_string *s);
typedef void (*button_fn)(void *button);

enum {
    GUI_QUICKBAR = 0x60,      /* CGuiInGame -> CPanelQuickBar* */
    PANEL_BUTTONS = 0xf8,     /* CGuiQuickButton[36], inline */
    PANEL_VISIBLE = 0x7298,   /* CGuiQuickButton*: first button of the visible bank */
    BUTTON_SIZE = 0x328,
    BUTTON_STRREF = 0x70,     /* tooltip strref (int, -1 none) */
    BUTTON_ITEM = 0x10c,      /* item object id */
    BUTTON_HAS_TEXT = 0x128,  /* nonzero: the tooltip isn't the strref */
    BUTTON_DATA = 0x130,      /* id word */
    BUTTON_ICON = 0x13c,      /* CResRef icon (16 chars) */
    BUTTON_TYPE = 0x160,
    BUTTON_LABEL = 0x188,     /* CExoString label (command buttons) */
    RULES_SPELLS = 0xd0,      /* CNWRules -> CNWSpellArray* */
};
enum { QB_EMPTY = 0, QB_ITEM = 1, QB_SPELL = 2, QB_COMMAND = 0x12, QB_SPELL_LIKE = 0x2c };

static void *panel(void) {
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    vcall_fn get_gui = (vcall_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_IN_GAME_GUI);
    if (!app_manager || !*app_manager || !get_gui) return NULL;
    void *app = *(void **)*app_manager;
    void *gui = app ? get_gui(app) : NULL;
    return gui ? *(void **)((char *)gui + GUI_QUICKBAR) : NULL;
}

static char *button(void *p, int slot) { return (char *)p + PANEL_BUTTONS + (size_t)slot * BUTTON_SIZE; }

/* Copy a game CExoString into out and free it the way the game does. */
static void take(exo_string *s, char *out, size_t cap) {
    snprintf(out, cap, "%s", s->ptr ? s->ptr : "");
    exo_string_dtor_fn dtor = (exo_string_dtor_fn)nwpad_sig(NWPAD_SIG_EXO_STRING_DTOR);
    if (dtor) dtor(s);
}

static void tlk(unsigned strref, char *out, size_t cap) {
    void **table = (void **)nwpad_sig(NWPAD_SIG_TLK_TABLE);
    tlk_string_fn get = (tlk_string_fn)nwpad_sig(NWPAD_SIG_TLK_GET_SIMPLE_STRING);
    out[0] = '\0';
    if (!table || !*table || !get) return;
    exo_string s = {0};
    get(&s, *table, strref);
    take(&s, out, cap);
}

/* Spell names as the quickbar tooltip builds them: metamagic prefix + name. */
static void spell_name(uint64_t data, char *out, size_t cap) {
    static const struct { unsigned bit, strref; } meta[] = {
        {1, 0x104ca}, {2, 0x104cb}, {4, 0x104cc}, {8, 0x104cd}, {0x10, 0x104ce}, {0x20, 0x104cf}};
    void **rules = (void **)nwpad_sig(NWPAD_SIG_RULES);
    get_spell_fn get_spell = (get_spell_fn)nwpad_sig(NWPAD_SIG_SPELL_ARRAY_GET_SPELL);
    string_of_fn name_of = (string_of_fn)nwpad_sig(NWPAD_SIG_SPELL_GET_NAME_TEXT);
    out[0] = '\0';
    if (!rules || !*rules || !get_spell || !name_of) return;
    void *spell = get_spell(*(void **)((char *)*rules + RULES_SPELLS), (int)(data & 0xffff));
    if (!spell) return;
    char prefix[64] = "", name[96];
    unsigned m = (unsigned)(data >> 0x1f) & 0x3f;
    for (size_t i = 0; i < sizeof meta / sizeof meta[0]; i++)
        if (m == meta[i].bit) tlk(meta[i].strref, prefix, sizeof prefix);
    exo_string s = {0};
    name_of(&s, spell);
    take(&s, name, sizeof name);
    snprintf(out, cap, "%s%s%s", prefix, prefix[0] ? " " : "", name);
}

static void item_name(uint32_t oid, char *out, size_t cap) {
    void **app_manager = (void **)nwpad_sig(NWPAD_SIG_APP_MANAGER);
    item_by_id_fn by_id = (item_by_id_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_ITEM_BY_ID);
    item_name_fn name_of = (item_name_fn)nwpad_sig(NWPAD_SIG_ITEM_GET_NAME);
    out[0] = '\0';
    if (!app_manager || !*app_manager || !by_id || !name_of) return;
    void *item = by_id(*(void **)*app_manager, oid);
    if (!item) return;
    exo_string s = {0};
    name_of(&s, item, 1);
    take(&s, out, cap);
}

bool nwpad_quickbar_read(nwpad_qb_slot out[NWPAD_QB_SLOTS]) {
    void *p = panel();
    if (!p) return false;
    for (int i = 0; i < NWPAD_QB_SLOTS; i++) {
        char *b = button(p, i);
        nwpad_qb_slot *s = &out[i];
        memset(s, 0, sizeof *s);
        s->type = (uint8_t)b[BUTTON_TYPE];
        memcpy(&s->data, b + BUTTON_DATA, sizeof s->data);
        memcpy(&s->item, b + BUTTON_ITEM, sizeof s->item);
        memcpy(s->icon, b + BUTTON_ICON, 16);
        if (s->type == QB_EMPTY) continue;
        if (s->type == QB_SPELL || s->type == QB_SPELL_LIKE) {
            spell_name(s->data, s->name, sizeof s->name);
        } else if (s->type == QB_ITEM) {
            item_name(s->item, s->name, sizeof s->name);
        } else if (s->type == QB_COMMAND) {
            exo_string *label = (exo_string *)(b + BUTTON_LABEL);
            snprintf(s->name, sizeof s->name, "%s", label->ptr ? label->ptr : "");
        } else {
            int32_t strref, has_text;
            memcpy(&strref, b + BUTTON_STRREF, sizeof strref);
            memcpy(&has_text, b + BUTTON_HAS_TEXT, sizeof has_text);
            if (!has_text && strref != -1) tlk((unsigned)strref, s->name, sizeof s->name);
        }
    }
    return true;
}

int nwpad_quickbar_bank(void) {
    void *p = panel();
    if (!p) return -1;
    char *visible = *(char **)((char *)p + PANEL_VISIBLE);
    long index = (visible - button(p, 0)) / BUTTON_SIZE;
    return index >= 0 && index < NWPAD_QB_SLOTS && index % 12 == 0 ? (int)(index / 12) : -1;
}

bool nwpad_quickbar_use(int slot) {
    void *p = panel();
    button_fn left = (button_fn)nwpad_sig(NWPAD_SIG_QUICKBUTTON_LEFT);
    if (!p || !left || slot < 0 || slot >= NWPAD_QB_SLOTS) return false;
    left(button(p, slot));
    return true;
}
