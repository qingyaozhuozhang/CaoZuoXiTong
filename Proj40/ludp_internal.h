/*
 * ludp_internal.h - internal definitions shared by the ludp translation units.
 */
#ifndef LUDP_INTERNAL_H
#define LUDP_INTERNAL_H

#define _GNU_SOURCE

#include "ludp.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define LUDP_MAX_PROBES       256
#define LUDP_RING_CAP_DEFAULT (1u << 22)   /* 4M events, power of two */
#define LUDP_RING_HEADER      256           /* padded header (cache-line sep)   */
#define LUDP_READY_OFF        256           /* ready-flags array offset         */
#define LUDP_EVENT_SZ         80
#define LUDP_EXEC_POOL_SIZE   (1u << 20)   /* 1 MiB trampoline arena (lazy) */
#define LUDP_MAX_CHAIN        64

/* Ring layout:
 *   [0,256)   header (seq / watermark / cap / overflow / mask / events_off)
 *   [256, 256+cap*8)   per-slot ready flags (sequence numbers, bulk-readable)
 *   [events_off, +cap*80)  event payloads (bulk-readable)
 * seq and the watermark live on separate cache lines so the CAS reservation
 * never ping-pongs with the watermark read; there is no commit spin at all. */
enum { RO_SEQ = 0, RO_COMMITTED = 64, RO_CAP = 128, RO_OVERFLOW = 136,
       RO_MASK = 144, RO_EVENTS_OFF = 152 };

typedef struct {
    int used;
    u64 id;
    char sym[128];
    char lib[128];
    uintptr_t addr;
    uintptr_t tramp;        /* trampoline entry address in the tracee */
    int orig_len;
    uint8_t orig[32];
    int chain_next;         /* index of the next (older) probe at same addr, -1 */
    int chain_prev;         /* index of the previous (newer) probe at same addr, -1 */
    bool enabled;
    u64 hits;
} probe_t;

typedef struct {
    int total_size;         /* tramp_end - tramp_start */
    int code_size;          /* data_begin - tramp_start */
    int reloc_off, reloc_size;
    int data_off;
    int off_ret, off_ring, off_pid;
    int off_rdfsbase;
} tramp_layout_t;

struct ludp_ctx {
    pid_t pid;
    int memfd;              /* /proc/<pid>/mem, O_RDWR */
    bool fsgsbase;
    uintptr_t syscall_insn; /* address of a `syscall` instruction in vDSO */

    int nthreads, cap_threads;
    pid_t *tids;            /* sorted; tids[0] == pid (leader) */
    bool *seized;

    uintptr_t ring_addr;
    u64 ring_cap, ring_mask;
    u64 events_off;         /* offset of the event-payload array */
    u64 consumed;           /* local drain cursor */
    u64 overflow;           /* cumulative dropped-event counter (tracer-side) */

    uintptr_t exec_base;    /* injected RWX trampoline pool (near shared libs) */
    uintptr_t exec_next;
    size_t exec_size;

    /* secondary RWX arena allocated *near* an out-of-range target (e.g. a
     * PIE main-program function ~0x55.. while the primary pool sits ~0x7f..).
     * Keeps the function-entry patch a compact 5-byte `jmp rel32`. */
    uintptr_t far_base;
    uintptr_t far_next;
    size_t far_size;
    bool far_valid;

    tramp_layout_t tlayout;
    probe_t probes[LUDP_MAX_PROBES];
    int nprobes;
    u64 next_id;

    bool attached;
    bool stopped;           /* whether we are currently in a quiesce() */
};

/* ---- memory ---- */
int  mem_read(ludp_ctx *c, uintptr_t addr, void *buf, size_t len);
int  mem_write(ludp_ctx *c, uintptr_t addr, const void *buf, size_t len);
u64  mem_read_u64(ludp_ctx *c, uintptr_t addr);

/* ---- remote syscall (operates on the stopped leader thread) ---- */
int  remote_mmap(ludp_ctx *c, size_t size, int prot, uintptr_t *out);
int  remote_mmap_near(ludp_ctx *c, uintptr_t target, size_t size, int prot,
                      uintptr_t *out);
int  remote_mprotect(ludp_ctx *c, uintptr_t addr, size_t len, int prot);

/* ---- threads ---- */
int  threads_refresh(ludp_ctx *c);
int  quiesce(ludp_ctx *c);
int  resume(ludp_ctx *c);
int  wait_stop(pid_t tid, int timeout_ms);

/* ---- trampoline ---- */
const tramp_layout_t *tramp_layout(void);
int  tramp_build(ludp_ctx *c, uintptr_t tramp_addr, u64 probe_id,
                 uintptr_t ring_addr, uintptr_t ret_addr,
                 const uint8_t *reloc_bytes, int reloc_len);

/* ---- instruction decoder ---- */
typedef struct {
    int len;
    int rip_off;    /* offset of RIP-relative disp32, or -1 */
    int rel_off;    /* offset of rel8/rel32 immediate, or -1 */
    int rel_size;   /* 1 or 4 */
} x86_insn;
int  x86_dec_one(const uint8_t *p, int maxlen, x86_insn *out);

/* ---- relocation ---- */
int  relocate_orig(const uint8_t *orig, int orig_len, uintptr_t orig_addr,
                   uintptr_t new_addr, uint8_t *out, int out_cap);

/* ---- elf ---- */
int  elf_resolve(pid_t pid, const char *lib, const char *sym, uintptr_t *addr,
                 char *path_out, size_t path_cap);

/* ---- probes (F6) ---- */
int  probe_add_internal(ludp_ctx *c, const char *sym, const char *lib,
                        uintptr_t addr, u64 *id_out);
int  probe_remove_internal(ludp_ctx *c, u64 id);
int  probe_set_enabled(ludp_ctx *c, u64 id, bool en);
int  rebuild_addr(ludp_ctx *c, uintptr_t addr);
int  find_probe(ludp_ctx *c, u64 id);
int  head_of(ludp_ctx *c, uintptr_t addr);

/* ---- events ---- */
int  events_read(ludp_ctx *c, ludp_event *out, int max, int *n);

/* ---- misc ---- */
u64  rdtsc_now(void);

#endif /* LUDP_INTERNAL_H */
