/*
 * probes.c - F6: probe registry, same-address chaining, enable/disable and
 * instruction patching. Every structural change happens under quiesce() so
 * the target is always in a consistent, multi-thread-safe state.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <sys/mman.h>
#include "ludp_internal.h"

int find_probe(ludp_ctx *c, u64 id)
{
    for (int i = 0; i < c->nprobes; i++)
        if (c->probes[i].used && c->probes[i].id == id)
            return i;
    return -1;
}

int head_of(ludp_ctx *c, uintptr_t addr)
{
    for (int i = 0; i < c->nprobes; i++)
        if (c->probes[i].used && c->probes[i].addr == addr &&
            c->probes[i].chain_prev == -1)
            return i;
    return -1;
}

/* Collect enabled probes at addr in newest->oldest order. */
static int collect_chain(ludp_ctx *c, uintptr_t addr, int *idx, int max)
{
    int n = 0;
    int cur = head_of(c, addr);
    while (cur != -1 && n < max) {
        if (c->probes[cur].used && c->probes[cur].enabled)
            idx[n++] = cur;
        cur = c->probes[cur].chain_next;
    }
    return n;
}

/* ---- reachable trampoline allocation -------------------------------- */

/* Prefer the primary pool, then reserve an arena within branch range. */
static uintptr_t tramp_alloc(ludp_ctx *c, uintptr_t addr, size_t need)
{
    uintptr_t t = (c->exec_next + 7) & ~(uintptr_t)7;
    if (t + need <= c->exec_base + c->exec_size && arch_branch_reaches(addr, t))
        return t;

    if (c->far_valid) {
        t = (c->far_next + 7) & ~(uintptr_t)7;
        if (t + need <= c->far_base + c->far_size && arch_branch_reaches(addr, t))
            return t;
    }

    size_t fa = need > 0x10000 ? need : 0x10000;
    fa = (fa + 0xfffUL) & ~(size_t)0xfffUL;
    uintptr_t base = 0;
    if (remote_mmap_near(c, addr, fa, PROT_READ | PROT_WRITE | PROT_EXEC,
                         &base) != 0)
        return 0;
    c->far_base = base;
    c->far_size = fa;
    c->far_next = base;
    c->far_valid = true;
    t = (base + 7) & ~(uintptr_t)7;
    return arch_branch_reaches(addr, t) ? t : 0;
}

/* Re-link the chain at addr: each enabled probe gets the right reloc bytes and
 * return target, and the probe site is patched to the newest enabled probe. */
int rebuild_addr(ludp_ctx *c, uintptr_t addr)
{
    const tramp_layout_t *tl = tramp_layout();
    int chain[LUDP_MAX_CHAIN];
    int n = collect_chain(c, addr, chain, LUDP_MAX_CHAIN);

    probe_t *first = NULL;
    for (int i = 0; i < c->nprobes; i++) {
        if (c->probes[i].used && c->probes[i].addr == addr) {
            first = &c->probes[i];
            break;
        }
    }
    if (!first)
        return 0;

    if (n == 0)
        return arch_write_patch(c, addr, 0, first->orig, first->orig_len);

    uint8_t reloc[32];
    for (int k = 0; k < n; k++) {
        probe_t *p = &c->probes[chain[k]];
        uintptr_t ret;
        /* NOP-fill the slot: relocate_orig only writes the first orig_len
         * bytes, the remainder must be benign NOPs, never stack garbage. */
        arch_fill_nops(reloc, sizeof reloc);
        if (k == n - 1) {
            /* oldest enabled probe: execute relocated originals and return */
            if (relocate_orig(first->orig, first->orig_len, addr,
                              p->tramp + tl->reloc_off, reloc, (int)sizeof reloc) < 0)
                return -1;
            ret = addr + first->orig_len;
        } else {
            ret = c->probes[chain[k + 1]].tramp;
        }
        if (mem_write_code(c, p->tramp + tl->reloc_off, reloc, (size_t)tl->reloc_size) != 0)
            return -1;
        if (arch_link_trampoline(c, p->tramp, ret) != 0)
            return -1;
    }

    return arch_write_patch(c, addr, c->probes[chain[0]].tramp,
                       first->orig, first->orig_len);
}

int probe_add_internal(ludp_ctx *c, const char *sym, const char *lib,
                       uintptr_t addr, unsigned flags, u64 *id_out)
{
    if (!c || (flags != LUDP_PROBE_ENTRY && flags != (LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN))) {
        errno = EINVAL; return -1;
    }
    int idx = 0;
    while (idx < c->nprobes && c->probes[idx].used) ++idx;
    if (c->closing || idx == LUDP_MAX_PROBES || addr == 0)
        return -1;
    bool was_stopped = c->stopped;
    if (quiesce(c) != 0) { fprintf(stderr, "[probe] quiesce fail\n"); return -1; }

#if LUDP_ARM64
    uint32_t first;
    if ((addr & 3) || mem_read(c, addr, &first, sizeof first)) {
        if (addr & 3) errno = EINVAL;
        if (!was_stopped) resume(c);
        return -1;
    }
    if ((first & UINT32_C(0xffffff3f)) == UINT32_C(0xd503241f))
        addr += sizeof first;  /* Keep BTI at the public entry. */
#endif
    int existing = head_of(c, addr);
    if (existing >= 0 && ((flags | c->probes[existing].flags) & LUDP_PROBE_RETURN)) {
        errno = ENOTSUP; if (!was_stopped) resume(c);
        return -1;
    }
    if ((flags & LUDP_PROBE_RETURN) && return_slots_init(c)) {
        if (!was_stopped) resume(c);
        return -1;
    }
    int previous_count = c->nprobes;
    probe_t *p = &c->probes[idx];
    memset(p, 0, sizeof *p);
    p->id = c->next_id++;
    snprintf(p->sym, sizeof p->sym, "%s", sym ? sym : "?");
    snprintf(p->lib, sizeof p->lib, "%s", lib ? lib : "");
    p->addr = addr;
    p->enabled = true;
    p->flags = flags;
    p->chain_next = -1;
    p->chain_prev = -1;

    int h = existing;
    if (h >= 0) {
        /* chained probe: share the original instruction bytes */
        memcpy(p->orig, c->probes[h].orig, sizeof p->orig);
        p->orig_len = c->probes[h].orig_len;
        p->chain_next = h;
        /* Link only after all preparation succeeds. */
    } else {
        p->orig_len = arch_read_entry(c, addr, p->orig);
        if (p->orig_len < 0) {
            if (!was_stopped) resume(c);
            return -1;
        }
    }

    const tramp_layout_t *tl = tramp_layout();
    /* A retired fixed-size slot can be reused once its prior probe drained. */
    retired_slot **reuse = &c->retired;
    while (*reuse && !arch_branch_reaches(addr, (*reuse)->addr)) reuse = &(*reuse)->next;
    p->tramp = *reuse ? (*reuse)->addr : tramp_alloc(c, addr, tl->total_size);
    if (!p->tramp) {
        fprintf(stderr, "[probe] no reachable exec mem sym=%s\n", sym);
        if (!was_stopped) resume(c);
        return -1; /* no reachable trampoline space */
    }

    uint8_t reloc[32];
    arch_fill_nops(reloc, sizeof reloc);
    if (tramp_build(c, p->tramp, p->id, c->ring_addr, addr + p->orig_len,
                    reloc, (int)sizeof reloc, flags) != 0) {
        fprintf(stderr, "[probe] tramp_build fail sym=%s\n", sym);
        if (!was_stopped) resume(c);
        return -1;
    }
    if (relocate_orig(p->orig, p->orig_len, addr, p->tramp + tl->reloc_off,
                      reloc, sizeof reloc) < 0) {
        p->used = 0; if (!was_stopped) resume(c);
        return -1;
    }
    if (h >= 0) c->probes[h].chain_prev = idx;
    p->used = 1;
    if (idx == c->nprobes) ++c->nprobes;

    if (rebuild_addr(c, addr) != 0) {
        fprintf(stderr, "[probe] rebuild fail sym=%s\n", sym);
        c->nprobes = previous_count;
        p->used = 0;
        if (h >= 0) { c->probes[h].chain_prev = -1; rebuild_addr(c, addr); }
        else arch_write_patch(c, addr, 0, p->orig, p->orig_len);
        if (!was_stopped) resume(c);
        return -1;
    }

    if (*reuse) {
        retired_slot *slot = *reuse;
        *reuse = slot->next;
        free(slot);
    } else if (c->far_valid && p->tramp >= c->far_base &&
               p->tramp < c->far_base + c->far_size) {
        c->far_next = p->tramp + tl->total_size;
    } else {
        c->exec_next = p->tramp + tl->total_size;
    }

    if (!was_stopped) resume(c);
    if (id_out)
        *id_out = p->id;
    return 0;
}

/* Commit the registry change only after the existing patch operation succeeds.
 * A failed rollback leaves the target stopped for a retry. */
static int switch_stopped(ludp_ctx *c, probe_t *p, bool enable)
{
    bool previous = p->enabled;
    if (previous == enable && !p->needs_rebuild) return 0;
    p->enabled = enable;
    if (!rebuild_addr(c, p->addr)) {
        p->needs_rebuild = false;
        return 0;
    }
    int err = errno;
    p->enabled = previous;
    p->needs_rebuild = rebuild_addr(c, p->addr) != 0;
    errno = p->needs_rebuild ? EIO : err;
    return -1;
}

int probe_remove_internal(ludp_ctx *c, u64 id)
{
    int i = c ? find_probe(c, id) : -1;
    if (i < 0) { errno = ENOENT; return -1; }
    bool running = !c->stopped;
    if (quiesce(c)) return -1;
    probe_t *p = &c->probes[i];
    bool last = !c->closing && ludp_probe_count(c) == 1;
    /* A return probe can still own a frame after its entry is disabled. */
    int rc = c->closing ? 0 : probe_storage_idle(c);
    retired_slot *slot = NULL;
    if (!rc && !last && !c->closing) {
        slot = malloc(sizeof *slot);
        if (!slot) rc = -1;
    }
    if (!rc) rc = switch_stopped(c, p, false);
    if (!rc && last) {
        p->retiring = true;
        rc = release_idle_storage(c, false);
    }
    if (!rc) {
        if (p->chain_prev >= 0)
            c->probes[p->chain_prev].chain_next = p->chain_next;
        if (p->chain_next >= 0)
            c->probes[p->chain_next].chain_prev = p->chain_prev;
        p->used = 0;
        if (slot) {
            *slot = (retired_slot){p->tramp, c->retired};
            c->retired = slot;
        }
        while (c->nprobes && !c->probes[c->nprobes - 1].used) --c->nprobes;
    } else {
        free(slot);
    }
    if (running && !p->needs_rebuild && resume(c)) return -1;
    return rc;
}

int probe_set_enabled(ludp_ctx *c, u64 id, bool en)
{
    int i = c ? find_probe(c, id) : -1;
    if (i < 0) { errno = ENOENT; return -1; }
    probe_t *p = &c->probes[i];
    if (c->closing || p->retiring) { errno = EBUSY; return -1; }
    if (p->enabled == en && !p->needs_rebuild) return 0;
    bool running = !c->stopped;
    if (quiesce(c)) return -1;
    int rc = switch_stopped(c, p, en);
    if (running && !p->needs_rebuild && resume(c)) return -1;
    return rc;
}
