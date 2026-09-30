/* See settings.h. */
#define _GNU_SOURCE
#include "settings.h"

#include "crashtrace.h"
#include "sigs.h"

#include <dlfcn.h>
#include <stdint.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool from_settings_tml; /* the [nwpad] section is the source: native entries allowed */

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[nwpad] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Read a whole file into a malloc'd, NUL-terminated buffer, or NULL. */
static char *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    for (size_t n; buf && (n = fread(buf + len, 1, cap - len - 1, f)) > 0;) {
        len += n;
        if (len + 1 == cap) {
            char *bigger = realloc(buf, cap *= 2);
            if (!bigger) { free(buf); buf = NULL; }
            else buf = bigger;
        }
    }
    fclose(f);
    if (buf) buf[len] = '\0';
    if (len_out) *len_out = len;
    return buf;
}

/* config.toml (the original format). Returns keys applied, or -1 if unreadable/malformed. */
static int load_config_toml(nwpad_config *cfg, const char *path) {
    char *text = slurp(path, NULL);
    if (!text) return -1;
    nwpad_config tmp = *cfg;
    int applied = nwpad_config_parse(&tmp, text);
    free(text);
    if (applied < 0) {
        say("config %s is malformed; ignored", path);
        return -1;
    }
    *cfg = tmp;
    say("config %s: %d setting(s) applied", path, applied);
    return applied;
}

static void config_toml_path(char *out, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(out, cap, "%s/nwpad/config.toml", xdg);
    else snprintf(out, cap, "%s/.config/nwpad/config.toml", home ? home : "");
}

/* The game's user directory: its -userdirectory argument, else the default. */
static void user_directory(char *out, size_t cap) {
    size_t len = 0;
    char *cmd = slurp("/proc/self/cmdline", &len);
    if (cmd) {
        for (size_t i = 0; i < len;) {
            const char *arg = cmd + i;
            size_t alen = strlen(arg);
            if (!strcmp(arg, "-userdirectory") && i + alen + 1 < len) {
                snprintf(out, cap, "%s", arg + alen + 1);
                free(cmd);
                return;
            }
            i += alen + 1;
        }
        free(cmd);
    }
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.local/share/Neverwinter Nights", home ? home : "");
}

/* Add the [nwpad] section to settings.tml atomically, backing the file up once. */
static int append_section(const char *path, const char *original, const char *section) {
    char backup[4200], tmp[4200];
    snprintf(backup, sizeof backup, "%s.bak-nwpad", path);
    snprintf(tmp, sizeof tmp, "%s.nwpad-tmp", path);
    if (access(backup, F_OK) != 0) {
        FILE *b = fopen(backup, "wb");
        if (!b || fputs(original, b) < 0 || fclose(b) != 0) return -1;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t olen = strlen(original);
    int ok = fputs(original, f) >= 0 && (olen == 0 || original[olen - 1] == '\n' || fputc('\n', f) != EOF) &&
             fputs(section, f) >= 0;
    if (fclose(f) != 0 || !ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

void nwpad_settings_load(nwpad_config *cfg) {
    nwpad_config_defaults(cfg);
    const char *explicit_path = getenv("NWPAD_CONFIG");
    if (explicit_path && *explicit_path) { /* tests and development: that file only */
        load_config_toml(cfg, explicit_path);
        return;
    }
    char toml_path[4096], dir[4000], settings[4096];
    config_toml_path(toml_path, sizeof toml_path);
    user_directory(dir, sizeof dir);
    snprintf(settings, sizeof settings, "%s/settings.tml", dir);

    char *text = slurp(settings, NULL);
    if (!text) { /* no settings.tml yet (first run) or unreadable: fall back this time */
        load_config_toml(cfg, toml_path);
        return;
    }
    int applied = nwpad_settings_parse(cfg, text);
    if (applied >= 0) {
        say("settings %s: %d setting(s) applied", settings, applied);
        from_settings_tml = true;
        free(text);
        return;
    }
    /* No [nwpad] section yet: import config.toml once (D2), then seed the section. */
    bool imported = load_config_toml(cfg, toml_path) >= 0;
    char section[1024];
    if (nwpad_settings_format(cfg, section, sizeof section) > 0 && append_section(settings, text, section) == 0) {
        say("added [nwpad] to %s (%s)", settings, imported ? "imported from config.toml" : "defaults");
        from_settings_tml = true;
    } else
        say("couldn't add [nwpad] to %s; using %s", settings, imported ? "config.toml" : "defaults");
    free(text);
}

/* ---- Native entries: the game's settings registry and Options window (re-notes F30) ----
 * The game is C++; these mirror the libstdc++ (cxx11 ABI) layouts it uses. */

typedef struct { char *ptr; size_t len; union { size_t cap; char buf[16]; } u; } cxx_string;
typedef struct { void *functor[2]; void *manager; void *invoker; } cxx_function; /* empty: manager NULL */
typedef struct { char *begin, *end, *cap; } cxx_vector;
typedef struct { bool empty; double value; } toml_option_d; /* cpptoml::option<double>, passed in registers */

typedef void *(*bind_bool_fn)(void *config, const cxx_string *key, const bool *def, cxx_function *on_change);
typedef void *(*bind_double_fn)(void *config, const cxx_string *key, const double *def, cxx_function *on_change);
typedef bool (*get_bool_fn)(void *config, const cxx_string *key, bool committed);
typedef double (*get_double_fn)(void *config, const cxx_string *key, bool committed);
typedef void *(*constrain_double_fn)(void *binding, toml_option_d min, toml_option_d max);
typedef void *(*step_double_fn)(void *binding, toml_option_d step);

/* Nui::ConfigWindow (F30): tabs at +0x1d8; ConfigTab and ConfigGroup are 0x58 bytes,
 * a group's main rows are the vector at +0x20; a ConfigOpt is 0x148 bytes: key string
 * at +0, label at +0x20, then flags and a 256-byte edit buffer. */
enum {
    WINDOW_TABS = 0x1d8, WINDOW_KEYS = 0x200, /* vector<std::string>: what Save commits, Cancel rolls back */
    TAB_SIZE = 0x58, TAB_GROUPS = 0x20,
    GROUP_SIZE = 0x58, GROUP_ROWS = 0x20,
    OPT_SIZE = 0x148, OPT_KEY = 0x00, OPT_LABEL = 0x20, OPT_SHOWN = 0x40, OPT_EDIT = 0x41, OPT_REFRESH = 0x141,
};
#define ANCHOR_KEY "camera.turn-speed-multiplier" /* our rows join the group holding this */

enum { E_ENABLED, E_HIDE_CURSOR, E_TURN, E_TILT, E_RUN_POINT, E_COUNT };
/* Steps must hold the defaults: Rollback (Cancel) re-applies the constraints, so a
 * default off the step grid would be moved. */
static const struct {
    const char *key, *label;
    bool is_bool;
    double min, max, step;
} entries[E_COUNT] = {
    [E_ENABLED] = {"nwpad.enabled", "Controller Support", true, 0, 0, 0},
    [E_HIDE_CURSOR] = {"nwpad.hide-cursor", "Controller Hides Cursor", true, 0, 0, 0},
    [E_TURN] = {"nwpad.camera.turn-speed", "Controller Camera Turn Speed", false, 60, 360, 10},
    [E_TILT] = {"nwpad.camera.tilt-speed", "Controller Camera Tilt Speed", false, 30, 180, 5},
    [E_RUN_POINT] = {"nwpad.movement.run-point", "Controller Run Point", false, 0.5, 0.95, 0.0125},
};

static struct {
    int state; /* 0 untried, 1 registered, -1 unavailable */
    void **exo_base;
    get_bool_fn get_bool;
    get_double_fn get_double;
    void *windows;           /* unordered_map<std::string, weak_ptr<Nui::Window>> */
    void *window_vptr;       /* vtable for enable_make<Nui::ConfigWindow>, + 0x10 */
    void *(*cxx_new)(size_t);
    void (*cxx_delete)(void *);
    cxx_string keys[E_COUNT];
    nwpad_config *cfg;       /* where change callbacks write */
    size_t window_count;     /* s_by_identifier size when last scanned */
    char *window_node;       /* the node the Options window was last found in */
    char *patched_window, *patched_tabs; /* last window given our rows (and its tab buffer) */
} nat;

/* A std::string the game may later destroy: heap storage from its operator new. */
static void cxx_string_init(cxx_string *s, const char *text) {
    size_t n = strlen(text);
    if (n < sizeof s->u.buf) {
        s->ptr = s->u.buf;
    } else {
        s->ptr = nat.cxx_new(n + 1);
        s->u.cap = n;
    }
    memcpy(s->ptr, text, n + 1);
    s->len = n;
}

static bool cxx_string_eq(const cxx_string *s, const char *text) {
    size_t n = strlen(text);
    return s->len == n && memcmp(s->ptr, text, n) == 0;
}

/* Move a std::string's bytes; a short (in-place) string must point at its new buffer. */
static void cxx_string_relocated(cxx_string *to, const cxx_string *from) {
    if (from->ptr == from->u.buf) to->ptr = to->u.buf;
}

/* Make room for `more` elements of `size` bytes in a game std::vector, moving the
 * elements like the vector would; `strings` lists each element's std::string
 * offsets, ending with (size_t)-1. */
static void cxx_vector_reserve(cxx_vector *v, size_t size, size_t more, const size_t *strings) {
    size_t n = (size_t)(v->end - v->begin) / size, need = n + more;
    if ((size_t)(v->cap - v->begin) / size >= need) return;
    char *grown = nat.cxx_new(need * size);
    for (size_t i = 0; i < n; i++) {
        char *to = grown + i * size, *from = v->begin + i * size;
        memcpy(to, from, size);
        for (const size_t *o = strings; *o != (size_t)-1; o++)
            cxx_string_relocated((cxx_string *)(to + *o), (cxx_string *)(from + *o));
    }
    if (v->begin) nat.cxx_delete(v->begin);
    v->begin = grown;
    v->end = grown + n * size;
    v->cap = grown + need * size;
}

/* Change callbacks: std::function<void(T)> built by hand (libstdc++ layout). The
 * functor is the entry index, stored in place; the game calls the invoker from
 * CExoConfig::Update() on its main loop whenever the value changes: Options rows,
 * reset, Cancel's rollback, and once after registration (F30). */
static bool on_change_manager(void *dest, const void *src, int op) {
    switch (op) {
    case 0: *(const void **)dest = NULL; break;          /* __get_type_info */
    case 1: *(const void **)dest = src; break;           /* __get_functor_ptr */
    case 2: memcpy(dest, src, 2 * sizeof(void *)); break; /* __clone_functor */
    default: break;                                       /* __destroy_functor */
    }
    return false;
}

static int entry_of(const void *functor) { return (int)(intptr_t)((void *const *)functor)[0]; }

static void on_change_double(const void *functor, double *value) {
    nwpad_settings_set_number(nat.cfg, entries[entry_of(functor)].key, *value);
}

static void on_change_bool(const void *functor, bool *value) {
    if (entry_of(functor) == E_ENABLED) nat.cfg->enabled = *value;
    else nat.cfg->hide_cursor = *value;
}

static bool native_register(void) {
    static const int ids[] = {NWPAD_SIG_EXO_BASE, NWPAD_SIG_CONFIG_BIND_BOOL, NWPAD_SIG_CONFIG_BIND_DOUBLE,
                              NWPAD_SIG_CONFIG_GET_BOOL, NWPAD_SIG_CONFIG_GET_DOUBLE,
                              NWPAD_SIG_CONFIG_CONSTRAIN_DOUBLE, NWPAD_SIG_CONFIG_STEP_DOUBLE,
                              NWPAD_SIG_NUI_WINDOWS, NWPAD_SIG_CONFIG_WINDOW_VTABLE};
    if (!nwpad_sigs_all(ids, sizeof ids / sizeof ids[0])) return false;
    nat.cxx_new = (void *(*)(size_t))dlsym(RTLD_DEFAULT, "_Znwm");
    nat.cxx_delete = (void (*)(void *))dlsym(RTLD_DEFAULT, "_ZdlPv");
    nat.exo_base = (void **)nwpad_sig(NWPAD_SIG_EXO_BASE);
    void *config = *nat.exo_base ? *(void **)*nat.exo_base : NULL;
    if (!nat.cxx_new || !nat.cxx_delete || !config) return false;
    nat.get_bool = (get_bool_fn)nwpad_sig(NWPAD_SIG_CONFIG_GET_BOOL);
    nat.get_double = (get_double_fn)nwpad_sig(NWPAD_SIG_CONFIG_GET_DOUBLE);
    nat.windows = nwpad_sig(NWPAD_SIG_NUI_WINDOWS);
    nat.window_vptr = (char *)nwpad_sig(NWPAD_SIG_CONFIG_WINDOW_VTABLE) + 0x10;
    bind_bool_fn bind_bool = (bind_bool_fn)nwpad_sig(NWPAD_SIG_CONFIG_BIND_BOOL);
    bind_double_fn bind_double = (bind_double_fn)nwpad_sig(NWPAD_SIG_CONFIG_BIND_DOUBLE);
    constrain_double_fn constrain = (constrain_double_fn)nwpad_sig(NWPAD_SIG_CONFIG_CONSTRAIN_DOUBLE);
    step_double_fn step = (step_double_fn)nwpad_sig(NWPAD_SIG_CONFIG_STEP_DOUBLE);

    /* Defaults as in §5; the value already in the game's table (our [nwpad] section,
     * loaded by the game) wins over them. */
    nwpad_config d;
    nwpad_config_defaults(&d);
    const double def_d[E_COUNT] = {[E_TURN] = d.camera_yaw_speed, [E_TILT] = d.camera_pitch_speed,
                                   [E_RUN_POINT] = d.run_threshold};
    const bool def_b[E_COUNT] = {[E_ENABLED] = d.enabled, [E_HIDE_CURSOR] = d.hide_cursor};
    for (int i = 0; i < E_COUNT; i++) {
        cxx_string_init(&nat.keys[i], entries[i].key); /* kept for Get; never freed */
        cxx_function on_change = {{(void *)(intptr_t)i, NULL}, (void *)on_change_manager,
                                  entries[i].is_bool ? (void *)on_change_bool : (void *)on_change_double};
        void *b = entries[i].is_bool ? bind_bool(config, &nat.keys[i], &def_b[i], &on_change)
                                     : bind_double(config, &nat.keys[i], &def_d[i], &on_change);
        if (!b) { /* already bound: someone else owns the key */
            say("native settings: %s is already registered; staying file-only", entries[i].key);
            return false;
        }
        if (!entries[i].is_bool) {
            constrain(b, (toml_option_d){false, entries[i].min}, (toml_option_d){false, entries[i].max});
            step(b, (toml_option_d){false, entries[i].step});
        }
    }
    return true;
}

/* Add our rows to an Options window's Camera group, once per window. */
static void native_patch_window(char *window) {
    cxx_vector *tabs = (cxx_vector *)(window + WINDOW_TABS);
    for (char *tab = tabs->begin; tab && tab < tabs->end; tab += TAB_SIZE) {
        cxx_vector *groups = (cxx_vector *)(tab + TAB_GROUPS);
        for (char *group = groups->begin; group && group < groups->end; group += GROUP_SIZE) {
            cxx_vector *rows = (cxx_vector *)(group + GROUP_ROWS);
            bool anchor = false;
            for (char *opt = rows->begin; opt && opt < rows->end; opt += OPT_SIZE) {
                if (cxx_string_eq((cxx_string *)(opt + OPT_KEY), entries[0].key)) return; /* done already */
                if (cxx_string_eq((cxx_string *)(opt + OPT_KEY), ANCHOR_KEY)) anchor = true;
            }
            if (!anchor) continue;
            static const size_t opt_strings[] = {OPT_KEY, OPT_LABEL, (size_t)-1};
            static const size_t key_strings[] = {0, (size_t)-1};
            cxx_vector *keys = (cxx_vector *)(window + WINDOW_KEYS);
            cxx_vector_reserve(rows, OPT_SIZE, E_COUNT, opt_strings);
            cxx_vector_reserve(keys, sizeof(cxx_string), E_COUNT, key_strings);
            for (int i = 0; i < E_COUNT; i++) {
                char *opt = rows->end;
                memset(opt, 0, OPT_SIZE);
                cxx_string_init((cxx_string *)(opt + OPT_KEY), entries[i].key);
                cxx_string_init((cxx_string *)(opt + OPT_LABEL), entries[i].label);
                opt[OPT_SHOWN] = 1;
                opt[OPT_EDIT] = 0;
                opt[OPT_REFRESH] = 1;
                rows->end += OPT_SIZE;
                cxx_string_init((cxx_string *)keys->end, entries[i].key);
                keys->end += sizeof(cxx_string);
            }
            say("native settings: added %d rows to the Options window", E_COUNT);
            return;
        }
    }
}

/* The live Options window in a Nui::Window::s_by_identifier node
 * { next, std::string key, weak_ptr<Window> { ptr, control block }, hash }, or NULL. */
static char *node_options_window(char *node) {
    char *window = *(char **)(node + 0x28);
    char *control = *(char **)(node + 0x30);
    if (!window || !control || *(int32_t *)(control + 8) <= 0) return NULL; /* closed */
    return *(void **)window == nat.window_vptr ? window : NULL;
}

/* The open Options window, if any. The map is only walked when its size changes;
 * otherwise the node the window was found in last time is checked (reopening it
 * reuses the node). */
static char *native_find_window(void) {
    size_t count = *(size_t *)((char *)nat.windows + 0x18); /* _M_element_count */
    if (count == nat.window_count) return nat.window_node ? node_options_window(nat.window_node) : NULL;
    nat.window_count = count;
    nat.window_node = NULL;
    for (char **node = *(char ***)((char *)nat.windows + 0x10); node; node = *(char ***)node) {
        char *window = node_options_window((char *)node);
        if (window) {
            nat.window_node = (char *)node;
            return window;
        }
    }
    return NULL;
}

void nwpad_settings_frame(nwpad_config *cfg) {
    if (nat.state == 0) {
        if (!from_settings_tml) {
            nat.state = -1;
            return;
        }
        NWPAD_WHERE("settings: register");
        nat.cfg = cfg;
        nat.state = native_register() ? 1 : -1;
        say(nat.state > 0 ? "native settings: registered (Options > Camera)"
                          : "native settings unavailable; settings.tml only");
        if (nat.state > 0) { /* read once; changes arrive through the callbacks */
            void *config = *(void **)*nat.exo_base;
            cfg->enabled = nat.get_bool(config, &nat.keys[E_ENABLED], false);
            cfg->hide_cursor = nat.get_bool(config, &nat.keys[E_HIDE_CURSOR], false);
            for (int i = E_TURN; i <= E_RUN_POINT; i++)
                nwpad_settings_set_number(cfg, entries[i].key, nat.get_double(config, &nat.keys[i], false));
        }
    }
    if (nat.state < 0) return;
    NWPAD_WHERE("settings: window");
    char *window = native_find_window();
    if (!window) return;
    char *tabs = *(char **)(window + WINDOW_TABS);
    if (window == nat.patched_window && tabs == nat.patched_tabs) return; /* rows already added */
    NWPAD_WHERE("settings: patch window");
    native_patch_window(window);
    nat.patched_window = window;
    nat.patched_tabs = tabs;
}

bool nwpad_settings_native(void) { return nat.state > 0; }

#ifdef NWPAD_DEBUG_SURFACES
/* Debug status: each native double key as working/committed. */
void nwpad_settings_debug_json(char *out, size_t cap) {
    if (nat.state <= 0) {
        snprintf(out, cap, "null");
        return;
    }
    void *config = *(void **)*nat.exo_base;
    size_t n = (size_t)snprintf(out, cap, "{");
    for (int i = E_TURN; i <= E_RUN_POINT && n < cap; i++)
        n += (size_t)snprintf(out + n, cap - n, "%s\"%s\":[%.4f,%.4f]", i == E_TURN ? "" : ",", entries[i].key,
                              nat.get_double(config, &nat.keys[i], false), nat.get_double(config, &nat.keys[i], true));
    if (n < cap) snprintf(out + n, cap - n, "}");
}
#endif
