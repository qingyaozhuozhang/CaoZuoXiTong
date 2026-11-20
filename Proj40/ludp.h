/*
 * ludp.h - Lightweight User-space Dynamic Probe
 *
 * Public API for the F6 (multi-probe coexistence) and F7 (multi-thread safety)
 * module. x86_64 / Linux only.
 *
 * License: MIT
 */
#ifndef LUDP_H
#define LUDP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t u64;

/* One event recorded by the in-process trampoline. */
typedef struct {
    u64 tsc;        /* rdtsc cycle counter (used for low-overhead timing)      */
    u64 tid;        /* per-thread TLS base via rdfsbase (stable thread id)     */
    u64 probe_id;   /* which probe fired                                       */
    u64 args[6];    /* SysV ABI argument registers rdi,rsi,rdx,rcx,r8,r9       */
    u64 ret_addr;   /* return address = probe addr + original instruction len  */
} ludp_event;

typedef struct ludp_ctx ludp_ctx;

typedef struct {
    u64 id;
    char sym[128];
    char lib[128];
    uintptr_t addr;
    uintptr_t trampoline;
    int orig_len;
    bool enabled;
    u64 hits;
} ludp_probe_info;

/* ---- lifecycle ---- */
/* Attach to a running process. Stops and traces all of its threads. */
ludp_ctx *ludp_attach(pid_t pid);
/* Remove all probes, restore original instructions, detach cleanly. */
void ludp_detach(ludp_ctx *ctx);

/* ---- symbol resolution ---- */
/* Resolve a symbol in the tracee. lib==NULL/"" means the main executable. */
int ludp_resolve(ludp_ctx *ctx, const char *lib, const char *sym, uintptr_t *addr);

/* ---- probes (F6) ---- */
/* Add a probe on a library symbol. Returns a new probe id via id_out. */
int ludp_probe_add(ludp_ctx *ctx, const char *lib, const char *sym, u64 *id_out);
/* Add a probe on an absolute address (caller resolved). */
int ludp_probe_add_addr(ludp_ctx *ctx, const char *sym, uintptr_t addr, u64 *id_out);
/* Remove one probe; restores original bytes when the address is left empty. */
int ludp_probe_remove(ludp_ctx *ctx, u64 id);
int ludp_probe_enable(ludp_ctx *ctx, u64 id);
int ludp_probe_disable(ludp_ctx *ctx, u64 id);
int ludp_probe_count(ludp_ctx *ctx);
int ludp_probe_list(ludp_ctx *ctx, ludp_probe_info *out, int max);

/* ---- events ---- */
/* Drain committed events into out (non-blocking; reads target memory live). */
int ludp_events_read(ludp_ctx *ctx, ludp_event *out, int max, int *n);
u64 ludp_overflow(ludp_ctx *ctx);   /* cumulative dropped-event counter */
u64 ludp_committed(ludp_ctx *ctx);  /* producer linearized commit index */

/* ---- direct memory access to the tracee ---- */
int ludp_mem_read(ludp_ctx *ctx, uintptr_t addr, void *buf, size_t len);
int ludp_mem_write(ludp_ctx *ctx, uintptr_t addr, const void *buf, size_t len);

/* ---- thread safety (F7) ---- */
/* Stop every thread of the tracee (quiescent state for safe patching). */
int ludp_quiesce(ludp_ctx *ctx);
/* Resume every thread. */
int ludp_resume(ludp_ctx *ctx);
/* Number of traced threads. */
int ludp_thread_count(ludp_ctx *ctx);
/* Drain runtime ptrace stops (clone/exit/signals) so newly created threads
 * keep running. Call it periodically while the tracee executes. Returns >0
 * for stops processed, 0 if idle, -1 if the tracee is gone. */
int ludp_poll(ludp_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LUDP_H */
