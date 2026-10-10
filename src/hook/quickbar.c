/* See quickbar.h. Layouts from re-notes F31. */
#include "quickbar.h"

#include "game.h"
#include "sigs.h"
#include "icons.h"
#include "../core/nwpad_core.h"

#include <stdio.h>
#include <string.h>

typedef nwpad_exo_string exo_string;

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
    void *gui = nwpad_in_game_gui();
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

static void *item_of(uint32_t oid) {
    item_by_id_fn by_id = (item_by_id_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_ITEM_BY_ID);
    void *app = by_id ? nwpad_client_app() : NULL;
    return app ? by_id(app, oid) : NULL;
}

/* Whether a C++ object's vtable pointer is that class's (the vtable symbol plus the
 * offset-to-top and typeinfo slots). */
static bool is_a(void *vptr, int vtable_sig) {
    char *vtable = nwpad_sig(vtable_sig);
    return vtable && vptr == vtable + 0x10;
}

/* An item's icon images, as its quickbar icon (CNWCItem+0x258, built by
 * CNWCItem::UpdateIcons) holds them (F34): CCompositeIcon parts at +0x08 (bottom),
 * +0x48 (middle), +0x59 (top); a plain CGuiIcon has one at +0x08. Others (armor) none. */
static void item_icon_parts(uint32_t oid, char parts[3][17]) {
    char *item = item_of(oid);
    char *icon = item ? *(char **)(item + 0x258) : NULL;
    if (!icon) return;
    void *vptr = *(void **)icon;
    static const size_t at[] = {0x08, 0x48, 0x59};
    bool layered = is_a(vptr, NWPAD_SIG_LAYERED_ICON_VTABLE);
    if (layered || is_a(vptr, NWPAD_SIG_ARMOR_ICON_VTABLE)) {
        /* Palette textures NUI can't draw: render them to an image (F35). */
        if (!nwpad_icon_render_plt(icon, layered ? 'L' : 'A', parts[0])) parts[0][0] = '\0';
        return;
    }
    int count = is_a(vptr, NWPAD_SIG_COMPOSITE_ICON_VTABLE) ? 3 : is_a(vptr, NWPAD_SIG_GUI_ICON_VTABLE) ? 1 : 0;
    for (int k = 0; k < count; k++) {
        memcpy(parts[k], icon + at[k], 16);
        parts[k][16] = '\0';
    }
}

static void item_name(uint32_t oid, char *out, size_t cap) {
    item_name_fn name_of = (item_name_fn)nwpad_sig(NWPAD_SIG_ITEM_GET_NAME);
    void *item = name_of ? item_of(oid) : NULL;
    out[0] = '\0';
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
            item_icon_parts(s->item, s->parts);
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
    if (!visible) return -1;
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


#ifdef NWPAD_DEBUG_SURFACES
bool nwpad_quickbar_debug_show_bank(int bank) {
    void *p = panel();
    void (*activate)(void *, unsigned char) = (void (*)(void *, unsigned char))nwpad_sig(NWPAD_SIG_QUICKBAR_ACTIVATE_SET);
    if (!p || !activate || bank < 0 || bank > 2) return false;
    activate(p, (unsigned char)bank);
    return true;
}

typedef void *(*player_fn)(void *app);
typedef uint32_t (*equipped_fn)(void *creature, unsigned slot_bit);

/* Debug: describe the icon object of the player's item in `slot_bit`: class,
 * resrefs and colour count (F34). */
void nwpad_quickbar_debug_equipped_icon(unsigned slot_bit, char *out, size_t cap) {
    player_fn pc_of = (player_fn)nwpad_sig(NWPAD_SIG_CLIENT_GET_PLAYER_CREATURE);
    equipped_fn equipped = (equipped_fn)nwpad_sig(NWPAD_SIG_CREATURE_EQUIPPED_ITEM);
    void *app = pc_of ? nwpad_client_app() : NULL;
    void *pc = app ? pc_of(app) : NULL;
    uint32_t oid = pc && equipped ? equipped(pc, slot_bit) : 0x7f000000;
    char *item = item_of(oid);
    char *icon = item ? *(char **)(item + 0x258) : NULL;
    if (!icon) {
        snprintf(out, cap, "{\"oid\":%u,\"icon\":null}", oid);
        return;
    }
    char *vptr = *(char **)icon;
    const char *cls = is_a(vptr, NWPAD_SIG_LAYERED_ICON_VTABLE)     ? "layered"
                      : is_a(vptr, NWPAD_SIG_ARMOR_ICON_VTABLE)     ? "armor"
                      : is_a(vptr, NWPAD_SIG_COMPOSITE_ICON_VTABLE) ? "composite"
                      : is_a(vptr, NWPAD_SIG_GUI_ICON_VTABLE)       ? "single"
                                                                                        : "?";
    char name[128];
    item_name(oid, name, sizeof name);
    char esc[160];
    nwpad_json_escape(esc, sizeof esc, name);
    size_t n = (size_t)snprintf(out, cap, "{\"oid\":%u,\"name\":\"%s\",\"class\":\"%s\",\"refs\":[", oid, esc, cls);
    size_t at[8], count = 0;
    at[count++] = 0x08;
    if (!strcmp(cls, "armor")) for (int k = 0; k < 6; k++) at[count++] = 0x48 + 0x11 * (size_t)k;
    for (size_t k = 0; k < count && n < cap; k++) {
        char ref[17];
        memcpy(ref, icon + at[k], 16);
        ref[16] = '\0';
        nwpad_json_escape(esc, sizeof esc, ref);
        n += (size_t)snprintf(out + n, cap - n, "%s\"%s\"", k ? "," : "", esc);
    }
    int colours = !strcmp(cls, "layered") ? *(int *)(icon + 0x48) : !strcmp(cls, "armor") ? *(int *)(icon + 0xb0) : 0;
    uint16_t *table = !strcmp(cls, "layered") ? *(uint16_t **)(icon + 0x50) : !strcmp(cls, "armor") ? *(uint16_t **)(icon + 0xb8) : NULL;
    if (n < cap) n += (size_t)snprintf(out + n, cap - n, "],\"colours\":[");
    for (int k = 0; table && k < colours && k < 16 && n < cap; k++)
        n += (size_t)snprintf(out + n, cap - n, "%s%u", k ? "," : "", table[k]);
    char rendered[17] = "";
    if (!strcmp(cls, "layered") || !strcmp(cls, "armor"))
        nwpad_icon_render_plt(icon, !strcmp(cls, "layered") ? 'L' : 'A', rendered);
    if (n < cap)
        snprintf(out + n, cap - n, "],\"rendered\":\"%s\",\"served\":%zu}", rendered,
                 rendered[0] ? nwpad_icon_debug_fetch(rendered) : 0);
}
#endif
