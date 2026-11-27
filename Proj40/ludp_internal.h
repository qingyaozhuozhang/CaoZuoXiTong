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

#if !defined(__x86_64__) && !defined(__aarch64__)
#error "LUDP supports Linux x86-64 and AArch64 only"
#endif

#define LUDP_MAX_PROBES       256
#define LUDP_RING_CAP_DEFAULT (1u << 22)   /* 4M events, power of two */
#define LUDP_RING_HEADER      256           /* padded header (cache-line sep)   */
#define LUDP_READY_OFF        256           /* ready-flags array offset         */
#define LUDP_EVENT_SZ         128
#define LUDP_RETURN_SLOTS     4096
#define LUDP_RETURN_SLOT_SZ   256
#define LUDP_EXEC_POOL_SIZE   (1u << 20)   /* 1 MiB trampoline arena (lazy) */
#define LUDP_MAX_CHAIN        LUDP_MAX_PROBES

/* Ring layout:
 *   [0,256)   header (seq / watermark / cap / overflow / mask / events_off)
 *   [256, 256+cap*8)   per-slot ready flags (sequence numbers, bulk-readable)
 *   [events_off, +cap*128) event payloads (bulk-readable)
 * seq and the watermark live on separate cache lines so the CAS reservation
 * never ping-pongs with the watermark read; there is no commit spin at all. */
enum { RO_SEQ = 0, RO_COMMITTED = 64, RO_CAP = 128, RO_OVERFLOW = 136,
       RO_MASK = 144, RO_EVENTS_OFF = 152 };

/* Remote ring ABI remains 128 bytes. Public ludp_event appends tracer-derived
 * CLOCK_MONOTONIC and kernel-TID fields after this raw payload. */
typedef struct {
    u64 tsc;
    u64 tid;
    u64 probe_id;
    u64 args[6];
    u64 ret_addr;
    u64 type;
    u64 retval;
    u64 retval_hi;
    u64 call_id;
    uint8_t xmm0[16];
} ludp_raw_event;

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
    bool needs_rebuild;
    bool retiring;
    unsigned flags;
    ludp_filter filter;
    ludp_call_action call;
    u64 generation;
    u64 hits;
} probe_t;

typedef struct {
    int total_size;         /* tramp_end - tramp_start */
    int code_size;          /* data_begin - tramp_start */
    int reloc_off, reloc_size;
    int data_off;
    int off_ret, off_ring, off_pid;
    int off_exit;          /* AArch64 direct B; avoids clobbering IP registers */
    int off_rdfsbase;
    int off_return_rdfsbase, off_slots, off_flags;
    int off_filter_op, off_filter_arg, off_filter_value, off_filter_mask;
    int off_call_fn, off_call_data, off_call_active;
    int off_xsave_mode, off_xsave_size, off_xsave_mask;
} tramp_layout_t;

typedef struct owned_mapping {
    uintptr_t base;
    size_t size;
    bool executable;
    struct owned_mapping *next;
} owned_mapping;

typedef struct retired_slot {
    uintptr_t addr;
    struct retired_slot *next;
} retired_slot;

struct ludp_ctx {
    retired_slot *retired;
    owned_mapping *mappings;
    bool slots_ready;
    bool closing;
    bool unsafe_state;     /* failed ptrace rollback: never resume scratch state */
    pid_t pid;
    int memfd;              /* /proc/<pid>/mem, O_RDWR */
    bool fsgsbase;
    bool xsave;
    u64 xsave_size;         /* 64-byte rounded FP/vector save area */
    u64 xsave_mask;         /* enabled x87/SSE/AVX/AVX-512 XCR0 bits */
    uintptr_t syscall_insn; /* syscall / svc instruction in the target vDSO */

    int nthreads, cap_threads;
    pid_t *tids;            /* sorted; tids[0] == pid (leader) */
    bool *seized;
    bool *thread_stopped;   /* ptrace-stop has been consumed for this tid */
    int *pending_sigs;      /* signal-delivery stops deferred by quiesce() */
    u64 *fsbases;           /* stopped-thread TLS base -> Linux TID mapping */

    uintptr_t ring_addr;
    u64 ring_cap, ring_mask;
    u64 events_off;         /* offset of the event-payload array */
    u64 consumed;           /* local drain cursor */
    u64 overflow;           /* cumulative dropped-event counter (tracer-side) */
    u64 clock_tsc_anchor;
    u64 clock_mono_anchor;
#if defined(__aarch64__)
    uint32_t counter_frequency;
#else
    long double clock_ns_per_tsc;
#endif
    bool clock_ready;

    uintptr_t exec_base;    /* injected RWX trampoline pool (near shared libs) */
    uintptr_t exec_next;
    size_t exec_size;
    uintptr_t return_slots;

    /* secondary RWX arena allocated *near* an out-of-range target (e.g. a
     * PIE main-program function ~0x55.. while the primary pool sits ~0x7f..).
     * Keeps the entry patch within the architecture's direct-branch range. */
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
                 const uint8_t *reloc_bytes, int reloc_len, unsigned flags,
                 const ludp_filter *filter, const ludp_call_action *call);

/* ---- instruction decoder ---- */
typedef struct {
    int len;
    int rip_off;    /* offset of RIP-relative disp32, or -1 */
    int rel_off;    /* offset of rel8/rel32 immediate, or -1 */
    int rel_size;   /* 1 or 4 */
    bool terminates; /* unconditional transfer: do not decode beyond it */
} x86_insn;
int  x86_dec_one(const uint8_t *p, int maxlen, x86_insn *out);

#ifdef __aarch64__
/* Return 0 on success, -1 with errno when unsupported or out of range. */
int aarch64_decode_prefix(const uint8_t *bytes, int available, int *length);
int aarch64_branch(uintptr_t from, uintptr_t to, uint32_t *instruction);
#endif

/* ---- relocation ---- */
int  relocate_orig(const uint8_t *orig, int orig_len, uintptr_t orig_addr,
                   uintptr_t new_addr, uint8_t *out, int out_cap);

/* ---- elf ---- */
int  elf_resolve(pid_t pid, const char *lib, const char *sym, uintptr_t *addr,
                 char *path_out, size_t path_cap);

/* ---- probes (F6) ---- */
int  probe_add_internal(ludp_ctx *c, const char *sym, const char *lib,
                        uintptr_t addr, unsigned flags,
                        const ludp_filter *filter,
                        const ludp_call_action *call, u64 *id_out);
int return_slots_init(ludp_ctx *c);
int probe_code_idle(ludp_ctx *c, bool *has_user_thread);
int probe_storage_idle(ludp_ctx *c);
int release_idle_storage(ludp_ctx *c, bool closing);
int  probe_remove_internal(ludp_ctx *c, u64 id);
int  probe_set_enabled(ludp_ctx *c, u64 id, bool en);
int  probe_update_internal(ludp_ctx *c, u64 id,
                           const ludp_probe_config *config);
int  filter_normalize(ludp_filter *filter);
int  call_action_normalize(ludp_ctx *c, ludp_call_action *call);
int  probe_callback_idle(ludp_ctx *c, probe_t *probe);
int  rebuild_addr(ludp_ctx *c, uintptr_t addr);
int  find_probe(ludp_ctx *c, u64 id);
int  head_of(ludp_ctx *c, uintptr_t addr);

/* ---- events ---- */
int  events_read(ludp_ctx *c, ludp_event *out, int max, int *n);

/* ---- misc ---- */
u64  rdtsc_now(void);

#endif /* LUDP_INTERNAL_H */
