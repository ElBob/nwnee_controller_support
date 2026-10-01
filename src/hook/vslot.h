/* Swapping one virtual-method slot in a game class's vtable (re-notes F30, F32):
 * the slot is found by value, so no index is hard-coded, and the vtable is in
 * RELRO, so the page is made writable just for the write. Nothing in the game's
 * code changes, like the SDL jump-table slots. */
#ifndef NWPAD_VSLOT_H
#define NWPAD_VSLOT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

/* vtable: the class's vtable symbol (its address points at offset-to-top, then
 * the typeinfo, then the slots). Replaces the first of `slots` slots equal to
 * `from` with `to`; *original gets `from`. */
static inline bool nwpad_vslot_swap(void *vtable, int slots, void *from, void *to, void **original) {
    void **slot = (void **)((char *)vtable + 0x10);
    for (int i = 0; i < slots; i++) {
        if (slot[i] != from) continue;
        long page = sysconf(_SC_PAGESIZE);
        void *start = (void *)((uintptr_t)&slot[i] & ~(uintptr_t)(page - 1));
        if (mprotect(start, (size_t)page, PROT_READ | PROT_WRITE) != 0) return false;
        *original = slot[i];
        slot[i] = to;
        mprotect(start, (size_t)page, PROT_READ);
        return true;
    }
    return false;
}

#endif
