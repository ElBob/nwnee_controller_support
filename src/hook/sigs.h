/* Signature resolver (plan §7): resolves every signatures/ee.yaml entry once at
 * load time. A missing signature disables the features that need it; it never
 * crashes the game (CLAUDE.md rule 5). */
#ifndef NWPAD_SIGS_H
#define NWPAD_SIGS_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *key, *note;
    const char *symbol;  /* exported name, or NULL */
    const char *pattern; /* byte pattern, or NULL */
    long offset;         /* patterns: added to the match address */
    int expect;          /* patterns: required number of matches */
} nwpad_sig_def;

#include "signatures_gen.h" /* NWPAD_SIG_* ids and nwpad_sig_defs, from the build */

/* Resolve all entries and log each miss. Call once, after the game is mapped. */
void nwpad_sigs_resolve(void);
/* Address for a signature id, or NULL if it didn't resolve. */
void *nwpad_sig(int id);
/* True if every listed id resolved. */
bool nwpad_sigs_all(const int *ids, size_t n);
/* JSON object: {"resolved":N,"total":M,"missing":["key",...]} */
void nwpad_sigs_json(char *out, size_t cap);

#endif
