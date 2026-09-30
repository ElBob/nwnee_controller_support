/* See settings.h. */
#define _GNU_SOURCE
#include "settings.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
        free(text);
        return;
    }
    /* No [nwpad] section yet: import config.toml once (D2), then seed the section. */
    bool imported = load_config_toml(cfg, toml_path) >= 0;
    char section[1024];
    if (nwpad_settings_format(cfg, section, sizeof section) > 0 && append_section(settings, text, section) == 0)
        say("added [nwpad] to %s (%s)", settings, imported ? "imported from config.toml" : "defaults");
    else
        say("couldn't add [nwpad] to %s; using %s", settings, imported ? "config.toml" : "defaults");
    free(text);
}
