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
#include <unistd.h>
#include "ludp_internal.h"

int filter_normalize(ludp_filter *filter)
{
    if (!filter) {
        errno = EINVAL;
        return -1;
    }
    if (filter->op > LUDP_FILTER_GE ||
        (filter->op != LUDP_FILTER_NONE && filter->arg_index >= 6)) {
        errno = EINVAL;
        return -1;
    }
    if (filter->op == LUDP_FILTER_NONE) {
        *filter = (ludp_filter){0};
    } else if (filter->op == LUDP_FILTER_MASK_EQ) {
        filter->value &= filter->mask;
    } else {
        filter->mask = 0;
    }
    return 0;
}

int call_action_normalize(ludp_ctx *c, ludp_call_action *call)
{
    if (!c || !call) {
        errno = EINVAL;
        return -1;
    }
    if (!call->function) {
        *call = (ludp_call_action){0};
        return 0;
    }

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", c->pid);
    FILE *maps = fopen(path, "r");
    if (!maps)
        return -1;
    char line[512], perms[5];
    unsigned long start, end;
    while (fgets(line, sizeof line, maps)) {
        if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3)
            continue;
        if (call->function >= (uintptr_t)start &&
            call->function < (uintptr_t)end) {
            fclose(maps);
            if (perms[2] == 'x')
                return 0;
            errno = EACCES;
            return -1;
        }
    }
    fclose(maps);
    errno = EACCES;
    return -1;
}

int probe_callback_idle(ludp_ctx *c, probe_t *p)
{
    if (!c || !p) {
        errno = EINVAL;
        return -1;
    }
    if (!p->call.function)
        return 0;
    const tramp_layout_t *tl = tramp_layout();
    u64 active = 0;
    if (mem_read(c, p->tramp + tl->off_call_active, &active, sizeof active))
        return -1;
    if (active) {
        errno = EBUSY;
        return -1;
    }
    return 0;
}

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

static void fill_nops(uint8_t *code, size_t size)
{
#ifdef __aarch64__
    const uint32_t nop = UINT32_C(0xd503201f);
    for (size_t i = 0; i + sizeof nop <= size; i += sizeof nop)
        memcpy(code + i, &nop, sizeof nop);
#else
    memset(code, 0x90, size);
#endif
}

/* Write a native direct branch, or restore original bytes when target==0. */
static int write_patch(ludp_ctx *c, uintptr_t addr, uintptr_t target,
                       const uint8_t *orig, int orig_len)
{
    uint8_t code[32];
    if (target == 0) {
        memcpy(code, orig, (size_t)orig_len);
        return mem_write(c, addr, code, (size_t)orig_len);
    }
    fill_nops(code, (size_t)orig_len);
#ifdef __aarch64__
    uint32_t branch;
    if (aarch64_branch(addr, target, &branch))
        return -1;
    memcpy(code, &branch, sizeof branch);
#else
    int64_t rel = (int64_t)target - (int64_t)(addr + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        errno = ERANGE;
        return -1;
    }
    code[0] = 0xE9;
    int32_t r32 = (int32_t)rel;
    memcpy(code + 1, &r32, 4);
#endif
    return mem_write(c, addr, code, (size_t)orig_len);
}

/* Collect enabled probes at addr in newest->oldest order. */
static int collect_chain(ludp_ctx *c, uintptr_t addr, int *idx, int max)
{
    int n = 0;
    int cur = head_of(c, addr);
    int visited = 0;
    while (cur != -1) {
        if (++visited > c->nprobes || n == max) {
            errno = ELOOP;
            return -1;
        }
        if (c->probes[cur].used && c->probes[cur].enabled)
            idx[n++] = cur;
        cur = c->probes[cur].chain_next;
    }
    return n;
}

/* ---- reachable trampoline allocation -------------------------------- */

static bool branch_reachable(uintptr_t from, uintptr_t to)
{
#ifdef __aarch64__
    int64_t d = (int64_t)to - (int64_t)from;
    /* Leave space for the direct branch back from the end of a trampoline. */
    return !(d & 3) && d > -(INT64_C(1) << 27) + 65536 &&
           d < (INT64_C(1) << 27) - 65536;
#else
    int64_t d = (int64_t)to - ((int64_t)from + 5);
    return d >= INT32_MIN && d <= INT32_MAX;
#endif
}

/* Reserve `need` bytes of RWX reachable from `addr` by a native direct branch.
 * Prefers the primary pool (allocated near shared libraries); if `addr`
 * is out of range (e.g. PIE main-program text ~0x55.. vs pool ~0x7f..),
 * it mmaps a secondary arena right next to `addr` so the entry patch
 * still works.  Returns 0 on failure. */
static uintptr_t tramp_alloc(ludp_ctx *c, uintptr_t addr, size_t need)
{
    uintptr_t t = (c->exec_next + 7) & ~(uintptr_t)7;
    if (t + need <= c->exec_base + c->exec_size && branch_reachable(addr, t))
        return t;

    if (c->far_valid) {
        t = (c->far_next + 7) & ~(uintptr_t)7;
        if (t + need <= c->far_base + c->far_size && branch_reachable(addr, t))
            return t;
    }

    size_t fa = need > 0x10000 ? need : 0x10000;
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) { errno = EINVAL; return 0; }
    fa = (fa + (size_t)page_size - 1) & ~((size_t)page_size - 1);
    uintptr_t base = 0;
    if (remote_mmap_near(c, addr, fa, PROT_READ | PROT_WRITE | PROT_EXEC,
                         &base) != 0)
        return 0;
    c->far_base = base;
    c->far_size = fa;
    c->far_next = base;
    c->far_valid = true;
    t = (base + 7) & ~(uintptr_t)7;
    return branch_reachable(addr, t) ? t : 0;
}

/* Re-link the chain at addr: each enabled probe gets the right reloc bytes and
 * return target, and the probe site is patched to the newest enabled probe. */
int rebuild_addr(ludp_ctx *c, uintptr_t addr)
{
    const tramp_layout_t *tl = tramp_layout();
    int chain[LUDP_MAX_CHAIN];
    int n = collect_chain(c, addr, chain, LUDP_MAX_CHAIN);
    if (n < 0)
        return -1;

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
        fill_nops(reloc, sizeof reloc);
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
#ifdef __aarch64__
        uint32_t branch;
        if (aarch64_branch(p->tramp + tl->off_exit, ret, &branch) ||
            mem_write(c, p->tramp + tl->off_exit, &branch, sizeof branch))
            return -1;
#endif
    }

    return write_patch(c, addr, c->probes[chain[0]].tramp,
                       first->orig, first->orig_len);
}

int probe_add_internal(ludp_ctx *c, const char *sym, const char *lib,
                       uintptr_t addr, unsigned flags,
                       const ludp_filter *filter,
                       const ludp_call_action *call, u64 *id_out)
{
    if (!c || !c->attached || !sym || !*sym || !addr ||
        (flags != LUDP_PROBE_ENTRY &&
         flags != (LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN))) {
        errno = EINVAL; return -1;
    }
    ludp_filter normalized = filter ? *filter : (ludp_filter){0};
    if (filter_normalize(&normalized))
        return -1;
    ludp_call_action normalized_call = call ? *call : (ludp_call_action){0};
    if (call_action_normalize(c, &normalized_call))
        return -1;
    int idx = 0;
    while (idx < c->nprobes && c->probes[idx].used) ++idx;
    if (c->closing) {
        errno = EBUSY;
        return -1;
    }
    if (idx == LUDP_MAX_PROBES) {
        errno = ENOSPC;
        return -1;
    }
    bool was_stopped = c->stopped;
    if (quiesce(c) != 0) { fprintf(stderr, "[probe] quiesce fail\n"); return -1; }
    /* /proc/<pid>/maps may change between the public API call and the safe
     * point.  Revalidate while every target thread is stopped before copying
     * the callback pointer into executable trampoline data. */
    if (normalized_call.function &&
        call_action_normalize(c, &normalized_call)) {
        int err = errno;
        if (!was_stopped) resume(c);
        errno = err;
        return -1;
    }

#ifdef __aarch64__
    /* Keep a BTI landing pad at the externally callable symbol address. */
    uint32_t first_instruction;
    if (mem_read(c, addr, &first_instruction, sizeof first_instruction)) {
        if (!was_stopped) resume(c);
        return -1;
    }
    if ((first_instruction & UINT32_C(0xffffff3f)) == UINT32_C(0xd503241f))
        addr += 4;
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
    p->filter = normalized;
    p->call = normalized_call;
    p->generation = 1;
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
        /* Read enough complete instructions for the architecture's branch. */
        uint8_t bytes[32];
#ifdef __aarch64__
        if (addr & 3) {
            errno = EINVAL;
            if (!was_stopped) resume(c);
            return -1;
        }
        const size_t prefix_size = 4;
#else
        const size_t prefix_size = sizeof bytes;
#endif
        if (mem_read(c, addr, bytes, prefix_size) != 0) {
            fprintf(stderr, "[probe] mem_read fail sym=%s\n", sym);
            if (!was_stopped) resume(c);
            return -1;
        }
        int total = 0;
#ifdef __aarch64__
        if (aarch64_decode_prefix(bytes, (int)prefix_size, &total)) {
            if (!was_stopped) resume(c);
            return -1;
        }
#else
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
#endif
        p->orig_len = total;
        memcpy(p->orig, bytes, (size_t)total);
    }

    const tramp_layout_t *tl = tramp_layout();
    /* A retired fixed-size slot can be reused once its prior probe drained. */
    retired_slot **reuse = &c->retired;
    while (*reuse && !branch_reachable(addr, (*reuse)->addr)) reuse = &(*reuse)->next;
    p->tramp = *reuse ? (*reuse)->addr : tramp_alloc(c, addr, tl->total_size);
    if (!p->tramp) {
        fprintf(stderr, "[probe] no reachable exec mem sym=%s\n", sym);
        if (!was_stopped) resume(c);
        return -1; /* no reachable trampoline space */
    }

    uint8_t reloc[32];
    fill_nops(reloc, sizeof reloc);
    if (relocate_orig(p->orig, p->orig_len, addr,
                      p->tramp + tl->reloc_off,
                      reloc, sizeof reloc) < 0) {
        p->used = 0;
        if (!was_stopped) resume(c);
        return -1;
    }
    /* A new head in an existing chain must fall through NOPs to the next
     * trampoline.  For the first probe, however, install the already
     * relocated instructions in the initial full-template write so the
     * publication step does not write the same fields a second time. */
    if (h >= 0)
        fill_nops(reloc, sizeof reloc);
    if (tramp_build(c, p->tramp, p->id, c->ring_addr, addr + p->orig_len,
                    reloc, (int)sizeof reloc, flags, &p->filter,
                    &p->call) != 0) {
        fprintf(stderr, "[probe] tramp_build fail sym=%s\n", sym);
        if (!was_stopped) resume(c);
        return -1;
    }
    if (h >= 0) c->probes[h].chain_prev = idx;
    p->used = 1;
    if (idx == c->nprobes) ++c->nprobes;

    int publish_rc = h >= 0
                         ? rebuild_addr(c, addr)
                         : write_patch(c, addr, p->tramp,
                                       p->orig, p->orig_len);
    if (publish_rc != 0) {
        fprintf(stderr, "[probe] rebuild fail sym=%s\n", sym);
        c->nprobes = previous_count;
        p->used = 0;
        if (h >= 0) { c->probes[h].chain_prev = -1; rebuild_addr(c, addr); }
        else write_patch(c, addr, 0, p->orig, p->orig_len);
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
    if (!rc) {
        p->generation++;
        if (!p->generation) p->generation = 1;
    }
    if (running && !p->needs_rebuild && resume(c)) return -1;
    return rc;
}

/* A5: replace a probe configuration while preserving its ID and trampoline
 * address. The tracee is stopped for the transaction; both the registry and
 * injected bytes are restored if any write or chain rebuild fails. */
int probe_update_internal(ludp_ctx *c, u64 id,
                          const ludp_probe_config *config)
{
    if (!c || !config ||
        (config->flags != LUDP_PROBE_ENTRY &&
         config->flags != (LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN))) {
        errno = EINVAL;
        return -1;
    }
    ludp_probe_config next = *config;
    if (filter_normalize(&next.filter))
        return -1;
    int i = find_probe(c, id);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    if (call_action_normalize(c, &next.call))
        return -1;
    probe_t *p = &c->probes[i];
    if (c->closing || p->retiring) {
        errno = EBUSY;
        return -1;
    }
    if (p->flags == next.flags && p->enabled == next.enabled &&
        memcmp(&p->filter, &next.filter, sizeof p->filter) == 0 &&
        memcmp(&p->call, &next.call, sizeof p->call) == 0)
        return 0;

    bool running = !c->stopped;
    if (quiesce(c))
        return -1;

    int rc = -1;
    int saved_errno = 0;
    const tramp_layout_t *tl = tramp_layout();
    uint8_t *backup = NULL;
    if (next.call.function && call_action_normalize(c, &next.call)) {
        saved_errno = errno;
        goto finish;
    }
    if (probe_callback_idle(c, p)) {
        saved_errno = errno;
        goto finish;
    }
    backup = malloc((size_t)tl->total_size);
    if (!backup) {
        saved_errno = errno;
        goto finish;
    }
    if (mem_read(c, p->tramp, backup, (size_t)tl->total_size)) {
        saved_errno = errno;
        goto finish;
    }

    if (p->flags != next.flags) {
        if ((p->flags | next.flags) & LUDP_PROBE_RETURN) {
            if (probe_storage_idle(c)) {
                saved_errno = errno;
                goto finish;
            }
        }
        for (int j = 0; j < c->nprobes; ++j) {
            probe_t *other = &c->probes[j];
            if (j != i && other->used && other->addr == p->addr &&
                ((other->flags | next.flags) & LUDP_PROBE_RETURN)) {
                saved_errno = ENOTSUP;
                goto finish;
            }
        }
        if ((next.flags & LUDP_PROBE_RETURN) && return_slots_init(c)) {
            saved_errno = errno;
            goto finish;
        }
    }

    probe_t previous = *p;
    p->flags = next.flags;
    p->enabled = next.enabled;
    p->filter = next.filter;
    p->call = next.call;
    p->generation++;
    if (!p->generation) p->generation = 1;

    uint8_t reloc[32];
    fill_nops(reloc, sizeof reloc);
    if (relocate_orig(p->orig, p->orig_len, p->addr,
                      p->tramp + tl->reloc_off, reloc, sizeof reloc) < 0 ||
        tramp_build(c, p->tramp, p->id, c->ring_addr,
                    p->addr + p->orig_len, reloc, sizeof reloc,
                    p->flags, &p->filter, &p->call) ||
        rebuild_addr(c, p->addr)) {
        saved_errno = errno ? errno : EIO;
        *p = previous;
        int restore_code = mem_write(c, p->tramp, backup,
                                     (size_t)tl->total_size);
        int restore_chain = rebuild_addr(c, p->addr);
        p->needs_rebuild = restore_code != 0 || restore_chain != 0;
        if (p->needs_rebuild)
            saved_errno = EIO;
        goto finish;
    }

    p->needs_rebuild = false;
    rc = 0;

finish:
    free(backup);
    if (running && !p->needs_rebuild && resume(c)) {
        if (!saved_errno) saved_errno = errno;
        rc = -1;
    }
    if (rc)
        errno = saved_errno ? saved_errno : EIO;
    return rc;
}
