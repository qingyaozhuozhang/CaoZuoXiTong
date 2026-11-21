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

/* Write a 5-byte `jmp rel32` (+ NOP padding), or the original bytes if target==0. */
static int write_patch(ludp_ctx *c, uintptr_t addr, uintptr_t target,
                       const uint8_t *orig, int orig_len)
{
    uint8_t code[32];
    if (target == 0) {
        memcpy(code, orig, (size_t)orig_len);
        return mem_write(c, addr, code, (size_t)orig_len);
    }
    int64_t rel = (int64_t)target - (int64_t)(addr + 5);
    if (rel < INT32_MIN || rel > INT32_MAX)
        return -1;
    code[0] = 0xE9;
    int32_t r32 = (int32_t)rel;
    memcpy(code + 1, &r32, 4);
    for (int i = 5; i < orig_len; i++)
        code[i] = 0x90;
    return mem_write(c, addr, code, (size_t)orig_len);
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

static bool in_rel32(uintptr_t from, uintptr_t to)
{
    int64_t d = (int64_t)to - ((int64_t)from + 5);
    return d >= INT32_MIN && d <= INT32_MAX;
}

/* Reserve `need` bytes of RWX reachable from `addr` by a jmp rel32.
 * Prefers the primary pool (allocated near shared libraries); if `addr`
 * is out of range (e.g. PIE main-program text ~0x55.. vs pool ~0x7f..),
 * it mmaps a secondary arena right next to `addr` so the 5-byte patch
 * still works.  Returns 0 on failure. */
static uintptr_t tramp_alloc(ludp_ctx *c, uintptr_t addr, size_t need)
{
    uintptr_t t = (c->exec_next + 7) & ~(uintptr_t)7;
    if (t + need <= c->exec_base + c->exec_size && in_rel32(addr, t))
        return t;

    if (c->far_valid) {
        t = (c->far_next + 7) & ~(uintptr_t)7;
        if (t + need <= c->far_base + c->far_size && in_rel32(addr, t))
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
    return in_rel32(addr, t) ? t : 0;
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
        return write_patch(c, addr, 0, first->orig, first->orig_len);

    uint8_t reloc[32];
    for (int k = 0; k < n; k++) {
        probe_t *p = &c->probes[chain[k]];
        uintptr_t ret;
        /* NOP-fill the slot: relocate_orig only writes the first orig_len
         * bytes, the remainder must be benign NOPs, never stack garbage. */
        memset(reloc, 0x90, sizeof reloc);
        if (k == n - 1) {
            /* oldest enabled probe: execute relocated originals and return */
            if (relocate_orig(first->orig, first->orig_len, addr,
                              p->tramp + tl->reloc_off, reloc, (int)sizeof reloc) < 0)
                return -1;
            ret = addr + first->orig_len;
        } else {
            ret = c->probes[chain[k + 1]].tramp;
        }
        if (mem_write(c, p->tramp + tl->reloc_off, reloc, (size_t)tl->reloc_size) != 0)
            return -1;
        if (mem_write(c, p->tramp + tl->off_ret, &ret, 8) != 0)
            return -1;
    }

    return write_patch(c, addr, c->probes[chain[0]].tramp,
                       first->orig, first->orig_len);
}

int probe_add_internal(ludp_ctx *c, const char *sym, const char *lib,
                       uintptr_t addr, unsigned flags, u64 *id_out)
{
    if (!c || (flags != LUDP_PROBE_ENTRY && flags != (LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN))) {
        errno = EINVAL; return -1;
    }
    if (c->nprobes >= LUDP_MAX_PROBES || addr == 0)
        return -1;
    bool was_stopped = c->stopped;
    if (quiesce(c) != 0) { fprintf(stderr, "[probe] quiesce fail\n"); return -1; }

    int existing = head_of(c, addr);
    if (existing >= 0 && ((flags | c->probes[existing].flags) & LUDP_PROBE_RETURN)) {
        errno = ENOTSUP; if (!was_stopped) resume(c);
        return -1;
    }
    if ((flags & LUDP_PROBE_RETURN) && return_slots_init(c)) {
        if (!was_stopped) resume(c);
        return -1;
    }
    int idx = c->nprobes;
    probe_t *p = &c->probes[idx];
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->id = c->next_id++;
    snprintf(p->sym, sizeof p->sym, "%s", sym ? sym : "?");
    snprintf(p->lib, sizeof p->lib, "%s", lib ? lib : "");
    p->addr = addr;
    p->enabled = true;
    p->flags = flags;
    p->chain_next = -1;
    p->chain_prev = -1;

    int h = head_of(c, addr);
    if (h >= 0) {
        /* chained probe: share the original instruction bytes */
        memcpy(p->orig, c->probes[h].orig, sizeof p->orig);
        p->orig_len = c->probes[h].orig_len;
        p->chain_next = h;
        /* Link only after all preparation succeeds. */
    } else {
        /* read original instructions until we cover a 5-byte jump */
        uint8_t bytes[32];
        if (mem_read(c, addr, bytes, sizeof bytes) != 0) {
            fprintf(stderr, "[probe] mem_read fail sym=%s\n", sym);
            if (!was_stopped) resume(c);
            return -1;
        }
        int total = 0;
        while (total < 5) {
            x86_insn insn;
            int l = x86_dec_one(bytes + total, (int)sizeof bytes - total, &insn);
            if (l <= 0 || total + l > (int)sizeof bytes ||
                (insn.terminates && total + l < 5)) {
                /* unsupported instruction at the probe site */
                fprintf(stderr, "[probe] decode fail sym=%s total=%d\n", sym, total);
                if (!was_stopped) resume(c);
                return -1;
            }
            total += l;
        }
        p->orig_len = total;
        memcpy(p->orig, bytes, (size_t)total);
    }

    const tramp_layout_t *tl = tramp_layout();
    p->tramp = tramp_alloc(c, addr, tl->total_size);
    if (!p->tramp) {
        fprintf(stderr, "[probe] no reachable exec mem sym=%s\n", sym);
        if (!was_stopped) resume(c);
        return -1; /* no reachable trampoline space */
    }

    uint8_t reloc[32];
    memset(reloc, 0x90, sizeof reloc);
    if (tramp_build(c, p->tramp, p->id, c->ring_addr, addr + p->orig_len,
                    reloc, (int)sizeof reloc, flags) != 0) {
        fprintf(stderr, "[probe] tramp_build fail sym=%s\n", sym);
        if (!was_stopped) resume(c);
        return -1;
    }
    if (c->far_valid && p->tramp >= c->far_base &&
        p->tramp < c->far_base + c->far_size)
        c->far_next = p->tramp + tl->total_size;
    else
        c->exec_next = p->tramp + tl->total_size;
    if (relocate_orig(p->orig, p->orig_len, addr, p->tramp + tl->reloc_off,
                      reloc, sizeof reloc) < 0) {
        p->used = 0; if (!was_stopped) resume(c);
        return -1;
    }
    if (h >= 0) c->probes[h].chain_prev = idx;
    c->nprobes = idx + 1;

    if (rebuild_addr(c, addr) != 0) {
        fprintf(stderr, "[probe] rebuild fail sym=%s\n", sym);
        c->nprobes = idx;
        p->used = 0;
        if (h >= 0) { c->probes[h].chain_prev = -1; rebuild_addr(c, addr); }
        else write_patch(c, addr, 0, p->orig, p->orig_len);
        if (!was_stopped) resume(c);
        return -1;
    }

    if (!was_stopped) resume(c);
    if (id_out)
        *id_out = p->id;
    return 0;
}

int probe_remove_internal(ludp_ctx *c, u64 id)
{
    int i = find_probe(c, id);
    if (i < 0)
        return -1;
    bool was_stopped = c->stopped;
    if (quiesce(c) != 0)
        return -1;

    probe_t *p = &c->probes[i];
    uintptr_t addr = p->addr;

    if (p->chain_prev >= 0)
        c->probes[p->chain_prev].chain_next = p->chain_next;
    if (p->chain_next >= 0)
        c->probes[p->chain_next].chain_prev = p->chain_prev;
    p->used = 0;

    bool any = false;
    for (int k = 0; k < c->nprobes; k++) {
        if (c->probes[k].used && c->probes[k].addr == addr) {
            any = true;
            break;
        }
    }
    if (any)
        rebuild_addr(c, addr);
    else
        write_patch(c, addr, 0, p->orig, p->orig_len);

    if (!was_stopped) resume(c);
    return 0;
}

int probe_set_enabled(ludp_ctx *c, u64 id, bool en)
{
    int i = find_probe(c, id);
    if (i < 0)
        return -1;
    if (c->probes[i].enabled == en)
        return 0;
    bool was_stopped = c->stopped;
    if (quiesce(c) != 0)
        return -1;

    c->probes[i].enabled = en;
    int rc = rebuild_addr(c, c->probes[i].addr);

    if (!was_stopped) resume(c);
    return rc;
}
