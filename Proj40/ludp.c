/*
 * ludp.c - core tracer: memory access, remote syscalls, thread quiescing,
 * trampoline construction, ring buffer and the attach/detach lifecycle.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <signal.h>
#include "ludp_internal.h"

#define PTRACE_OPTS (PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL)

/* trampoline template symbols (copied verbatim into the tracee) */
extern const char ludp_tramp_start[], ludp_tramp_end[];
extern const char ludp_tramp_code_end[];
extern const char ludp_tramp_data_begin[], ludp_tramp_data_end[];
extern const char ludp_tramp_reloc_begin[], ludp_tramp_reloc_end[];
extern const char ludp_tramp_ret_addr[], ludp_tramp_ring_ptr[];
extern const char ludp_tramp_probe_id[], ludp_tramp_rdfsbase[];

/* ============================ memory access ============================ */

int mem_read(ludp_ctx *c, uintptr_t addr, void *buf, size_t len)
{
    if (len == 0)
        return 0;
    ssize_t r = pread(c->memfd, buf, len, (off_t)addr);
    if (r == (ssize_t)len)
        return 0;
    fprintf(stderr, "[mem_read] pread len=%zu got=%zd errno=%d addr=0x%lx\n",
            len, r, errno, (unsigned long)addr);

    /* fallback: PTRACE_PEEKDATA (requires a stopped tracee) */
    uint8_t *b = buf;
    for (size_t off = 0; off < len; off += 8) {
        size_t chunk = len - off < 8 ? len - off : 8;
        errno = 0;
        long w = ptrace(PTRACE_PEEKDATA, c->tids[0], (void *)(addr + off), 0);
        if (errno)
            return -1;
        memcpy(b + off, &w, chunk);
    }
    return 0;
}

int mem_write(ludp_ctx *c, uintptr_t addr, const void *buf, size_t len)
{
    if (len == 0)
        return 0;
    ssize_t r = pwrite(c->memfd, buf, len, (off_t)addr);
    if (r == (ssize_t)len)
        return 0;

    /* fallback: PTRACE_POKEDATA, read-modify-write for unaligned tails */
    const uint8_t *b = buf;
    for (size_t off = 0; off < len; off += 8) {
        size_t chunk = len - off < 8 ? len - off : 8;
        long w;
        if (chunk < 8) {
            errno = 0;
            long old = ptrace(PTRACE_PEEKDATA, c->tids[0], (void *)(addr + off), 0);
            if (errno)
                return -1;
            uint8_t tmp[8];
            memcpy(tmp, &old, 8);
            memcpy(tmp, b + off, chunk);
            memcpy(&w, tmp, 8);
        } else {
            memcpy(&w, b + off, 8);
        }
        if (ptrace(PTRACE_POKEDATA, c->tids[0], (void *)(addr + off), w) != 0)
            return -1;
    }
    return 0;
}

u64 mem_read_u64(ludp_ctx *c, uintptr_t addr)
{
    u64 v = 0;
    mem_read(c, addr, &v, 8);
    return v;
}

/* ============================ threads ============================ */

static int tid_index(ludp_ctx *c, pid_t tid)
{
    for (int i = 0; i < c->nthreads; i++)
        if (c->tids[i] == tid)
            return i;
    return -1;
}

static void remove_thread(ludp_ctx *c, pid_t tid)
{
    int i = tid_index(c, tid);
    if (i < 0)
        return;
    c->tids[i] = c->tids[c->nthreads - 1];
    c->seized[i] = c->seized[c->nthreads - 1];
    c->nthreads--;
}


static int add_thread(ludp_ctx *c, pid_t tid)
{
    int i = tid_index(c, tid);
    if (i >= 0)
        return i;
    if (c->nthreads == c->cap_threads) {
        int nc = c->cap_threads ? c->cap_threads * 2 : 32;
        c->tids = realloc(c->tids, (size_t)nc * sizeof(pid_t));
        c->seized = realloc(c->seized, (size_t)nc * sizeof(bool));
        if (!c->tids || !c->seized)
            return -1;
        c->cap_threads = nc;
    }
    c->tids[c->nthreads] = tid;
    c->seized[c->nthreads] = false;
    return c->nthreads++;
}

static int cmp_pid(const void *a, const void *b)
{
    pid_t x = *(const pid_t *)a, y = *(const pid_t *)b;
    return (x > y) - (x < y);
}

int threads_refresh(ludp_ctx *c)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", c->pid);
    DIR *d = opendir(path);
    if (!d)
        return -1;

    /* Snapshot the live thread ids first, then reconcile against our list. */
    pid_t cur[4096];
    int ncur = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && ncur < 4096) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        cur[ncur++] = (pid_t)atoi(e->d_name);
    }
    closedir(d);

    /* Drop threads that no longer exist (they exited during a run phase). */
    for (int i = c->nthreads - 1; i >= 0; i--) {
        bool alive = false;
        for (int k = 0; k < ncur; k++) {
            if (cur[k] == c->tids[i]) { alive = true; break; }
        }
        if (!alive) {
            int st;
            waitpid(c->tids[i], &st, __WALL | WNOHANG);  /* reap zombie */
            remove_thread(c, i);
        }
    }

    /* Add any threads we have not seen before. */
    for (int k = 0; k < ncur; k++)
        add_thread(c, cur[k]);

    qsort(c->tids, (size_t)c->nthreads, sizeof(pid_t), cmp_pid);

    /* keep the leader (tgid) first so remote syscalls run on it */
    int li = tid_index(c, c->pid);
    if (li > 0) {
        pid_t t = c->tids[0]; c->tids[0] = c->tids[li]; c->tids[li] = t;
        bool s = c->seized[0]; c->seized[0] = c->seized[li]; c->seized[li] = s;
    }

    for (int i = 0; i < c->nthreads; i++) {
        if (!c->seized[i]) {
            if (ptrace(PTRACE_SEIZE, c->tids[i], 0, PTRACE_OPTS) == 0)
                c->seized[i] = true;
        }
    }
    return 0;
}

int wait_stop(pid_t tid, int timeout_ms)
{
    int st;
    for (int i = 0; i < timeout_ms; i++) {
        pid_t w = waitpid(tid, &st, __WALL | WNOHANG);
        if (w == tid)
            return st;
        if (w < 0)
            return -1;
        usleep(1000);
    }
    return -1;
}

int quiesce(ludp_ctx *c)
{
    if (c->stopped)
        return 0;
    threads_refresh(c);

    /* "pending" holds every live tid we are waiting to see stop (or exit).
     * Keeping it index-free avoids the pitfalls of stale array positions when
     * a thread exits and is removed mid-quiesce. */
    pid_t *pending = malloc((size_t)c->nthreads * sizeof(pid_t));
    if (!pending)
        return -1;
    int remain = 0;
    for (int i = 0; i < c->nthreads; i++) {
        pending[remain++] = c->tids[i];
        ptrace(PTRACE_INTERRUPT, c->tids[i], 0, 0);
    }

    int deadline = 3000; /* ms */

    while (remain > 0 && deadline > 0) {
        int st;
        pid_t w = waitpid(-1, &st, __WALL | WNOHANG);
        if (w > 0) {
            if (WIFSTOPPED(st)) {
                if (((st >> 16) & 0xffff) == PTRACE_EVENT_CLONE) {
                    unsigned long nt = 0;
                    if (ptrace(PTRACE_GETEVENTMSG, w, 0, &nt) == 0 && nt > 0) {
                        int ni = add_thread(c, (pid_t)nt);
                        if (ni >= 0) {
                            c->seized[ni] = true;
                            pid_t *np = realloc(pending,
                                                (size_t)c->nthreads * sizeof(pid_t));
                            if (np) {
                                pending = np;
                                pending[remain++] = (pid_t)nt;
                            }
                            ptrace(PTRACE_INTERRUPT, (pid_t)nt, 0, 0);
                        }
                    }
                }
                for (int k = 0; k < remain; k++) {
                    if (pending[k] == w) {
                        pending[k] = pending[remain - 1];
                        remain--;
                        break;
                    }
                }
            } else if (WIFEXITED(st) || WIFSIGNALED(st)) {
                /* a thread died while we were stopping it: drop it for good */
                remove_thread(c, w);
                for (int k = 0; k < remain; k++) {
                    if (pending[k] == w) {
                        pending[k] = pending[remain - 1];
                        remain--;
                        break;
                    }
                }
            }
            continue;
        }
        usleep(1000);
        deadline--;
    }
    free(pending);

    if (remain > 0)
        return -1; /* a thread failed to stop -> potential deadlock detected */
    c->stopped = true;
    return 0;
}

int resume(ludp_ctx *c)
{
    if (!c->stopped)
        return 0;
    for (int i = 0; i < c->nthreads; i++)
        ptrace(PTRACE_CONT, c->tids[i], 0, 0);
    c->stopped = false;
    return 0;
}

/* Reap pending ptrace stops while the tracee is running and keep dynamically
 * created threads going (F7). This is the runtime counterpart of quiesce():
 * quiesce() stops everything for a structural change, poll() drains the
 * clone/exit/signal stops that accumulate while the tracee freely executes.
 *
 * Returns the number of stops processed (>0), 0 if nothing was pending, or -1
 * if the tracee has no live threads left. */
int ludp_poll(ludp_ctx *c)
{
    int n = 0;
    if (!c || c->stopped)
        return 0;
    for (;;) {
        int st;
        pid_t w = waitpid(-1, &st, __WALL | WNOHANG);
        if (w <= 0)
            break;
        n++;

        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            remove_thread(c, w);
            continue;
        }
        if (!WIFSTOPPED(st))
            continue;

        int ev = (st >> 16) & 0xffff;
        if (ev == PTRACE_EVENT_CLONE) {
            /* pthread_create(): new thread is already traced + stopped. */
            unsigned long nt = 0;
            if (ptrace(PTRACE_GETEVENTMSG, w, 0, &nt) == 0 && nt > 0) {
                int ni = add_thread(c, (pid_t)nt);
                if (ni >= 0)
                    c->seized[ni] = true;   /* attached via TRACECLONE */
            }
        }

        /* Event stops are synthetic SIGTRAPs and must be resumed with no
         * signal; a genuine signal-delivery stop (e.g. SIGSEGV) is forwarded
         * so a crash is not silently swallowed. */
        int sig = (ev != 0 || WSTOPSIG(st) == SIGSTOP || WSTOPSIG(st) == SIGTRAP)
                      ? 0 : WSTOPSIG(st);
        ptrace(PTRACE_CONT, w, 0, sig);
    }
    return c->nthreads > 0 ? n : -1;
}

/* ============================ remote syscalls ============================ */

static uintptr_t find_syscall_insn(ludp_ctx *c)
{
    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/maps", c->pid);
    FILE *f = fopen(mp, "r");
    if (!f)
        return 0;
    char line[1024];
    uintptr_t vs = 0, ve = 0;
    while (fgets(line, sizeof line, f)) {
        uintptr_t a, b;
        char path[256] = {0};
        if (sscanf(line, "%lx-%lx %*s %*s %*s %*s %255s", &a, &b, path) >= 2 &&
            strstr(path, "vdso")) {
            vs = a; ve = b; break;
        }
    }
    fclose(f);
    if (!vs)
        return 0;

    size_t len = ve - vs;
    uint8_t *buf = malloc(len);
    if (!buf)
        return 0;
    if (mem_read(c, vs, buf, len) != 0) {
        free(buf);
        return 0;
    }
    uintptr_t found = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == 0x0f && buf[i + 1] == 0x05) {
            found = vs + i;
            break;
        }
    }
    free(buf);
    return found;
}

static int remote_syscall(ludp_ctx *c, long nr, unsigned long a1, unsigned long a2,
                          unsigned long a3, unsigned long a4, unsigned long a5,
                          unsigned long a6, uintptr_t *ret)
{
    pid_t tid = c->tids[0];
    struct user_regs_struct orig, regs;

    if (ptrace(PTRACE_GETREGS, tid, 0, &orig) != 0)
        return -1;
    regs = orig;
    regs.rax = nr;
    regs.rdi = a1; regs.rsi = a2; regs.rdx = a3;
    regs.r10 = a4; regs.r8 = a5; regs.r9 = a6;
    regs.rip = c->syscall_insn;
    if (ptrace(PTRACE_SETREGS, tid, 0, &regs) != 0)
        return -1;

    if (ptrace(PTRACE_SYSCALL, tid, 0, 0) != 0)
        return -1;
    if (wait_stop(tid, 3000) < 0)   /* syscall entry */
        return -1;

    if (ptrace(PTRACE_GETREGS, tid, 0, &regs) != 0)
        return -1;
    regs.rip = orig.rip;            /* redirect the return safely */
    if (ptrace(PTRACE_SETREGS, tid, 0, &regs) != 0)
        return -1;

    if (ptrace(PTRACE_SYSCALL, tid, 0, 0) != 0)
        return -1;
    if (wait_stop(tid, 3000) < 0)   /* syscall exit */
        return -1;

    if (ptrace(PTRACE_GETREGS, tid, 0, &regs) != 0)
        return -1;
    if (ret)
        *ret = regs.rax;
    if (ptrace(PTRACE_SETREGS, tid, 0, &orig) != 0)
        return -1;
    return 0;
}

int remote_mmap(ludp_ctx *c, size_t size, int prot, uintptr_t *out)
{
    uintptr_t ret = 0;
    int rc = remote_syscall(c, __NR_mmap, 0, size, prot,
                            MAP_PRIVATE | MAP_ANONYMOUS,
                            (unsigned long)-1, 0, &ret);
    if (rc != 0)
        return -1;
    if ((long)ret < 0)
        return -1;
    if (out)
        *out = ret;
    return 0;
}

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/*
 * Allocate an RWX arena within ~±1.9 GB of `target`, so the compact 5-byte
 * `jmp rel32` at `target` can reach it.  Must be called while the tracee's
 * threads are stopped (same context as remote_mmap during attach).
 */
int remote_mmap_near(ludp_ctx *c, uintptr_t target, size_t size, int prot,
                     uintptr_t *out)
{
    size = (size + 0xfffUL) & ~(size_t)0xfffUL;
    if (!size)
        size = 0x1000;

    enum { MAXMAP = 512 };
    uintptr_t lo[MAXMAP], hi[MAXMAP];
    int nm = 0;
    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/maps", c->pid);
    FILE *f = fopen(mp, "r");
    if (!f)
        return -1;
    char line[512];
    while (nm < MAXMAP && fgets(line, sizeof line, f)) {
        unsigned long a = 0, b = 0;
        if (sscanf(line, "%lx-%lx", &a, &b) == 2) {
            lo[nm] = a;
            hi[nm] = b;
            nm++;
        }
    }
    fclose(f);

    const uintptr_t LIMIT = 0x78000000UL;   /* 1.875 GB, headroom for addr+5 */
    const uintptr_t STEP = 0x100000UL;      /* 1 MB scan granularity */

    for (int dir = 0; dir < 2; dir++) {
        for (uintptr_t off = 0; off <= LIMIT; off += STEP) {
            int64_t cand = dir == 0 ? (int64_t)target + (int64_t)off
                                    : (int64_t)target - (int64_t)off;
            if (cand <= 0x10000)
                continue;
            uintptr_t p = ((uintptr_t)cand + 0xfffUL) & ~(uintptr_t)0xfffUL;
            int64_t d = (int64_t)p - (int64_t)target;
            if (d < -(int64_t)LIMIT || d > (int64_t)LIMIT)
                continue;
            int busy = 0;
            for (int i = 0; i < nm; i++) {
                if (p < hi[i] && p + size > lo[i]) { busy = 1; break; }
            }
            if (busy)
                continue;
            uintptr_t ret = 0;
            int rc = remote_syscall(c, __NR_mmap, p, size, prot,
                                    MAP_PRIVATE | MAP_ANONYMOUS |
                                        MAP_FIXED_NOREPLACE,
                                    (unsigned long)-1, 0, &ret);
            if (rc == 0 && ret == p) {
                if (out)
                    *out = p;
                return 0;
            }
        }
    }
    return -1;
}

int remote_mprotect(ludp_ctx *c, uintptr_t addr, size_t len, int prot)
{
    uintptr_t ret = 0;
    if (remote_syscall(c, __NR_mprotect, addr, len, prot, 0, 0, 0, &ret) != 0)
        return -1;
    return (long)ret == 0 ? 0 : -1;
}

/* ============================ trampoline ============================ */

const tramp_layout_t *tramp_layout(void)
{
    static tramp_layout_t l;
    static int init = 0;
    if (!init) {
        l.total_size = (int)(ludp_tramp_end - ludp_tramp_start);
        l.code_size = (int)(ludp_tramp_code_end - ludp_tramp_start);
        l.reloc_off = (int)(ludp_tramp_reloc_begin - ludp_tramp_start);
        l.reloc_size = (int)(ludp_tramp_reloc_end - ludp_tramp_reloc_begin);
        l.data_off = (int)(ludp_tramp_data_begin - ludp_tramp_start);
        l.off_ret = (int)(ludp_tramp_ret_addr - ludp_tramp_start);
        l.off_ring = (int)(ludp_tramp_ring_ptr - ludp_tramp_start);
        l.off_pid = (int)(ludp_tramp_probe_id - ludp_tramp_start);
        l.off_rdfsbase = (int)(ludp_tramp_rdfsbase - ludp_tramp_start);
        init = 1;
    }
    return &l;
}

int tramp_build(ludp_ctx *c, uintptr_t tramp_addr, u64 probe_id,
                uintptr_t ring_addr, uintptr_t ret_addr,
                const uint8_t *reloc_bytes, int reloc_len)
{
    const tramp_layout_t *tl = tramp_layout();
    int total = tl->total_size;
    uint8_t *buf = malloc((size_t)total);
    if (!buf)
        return -1;

    memcpy(buf, ludp_tramp_start, (size_t)total);

    memcpy(buf + tl->off_ret, &ret_addr, 8);
    memcpy(buf + tl->off_ring, &ring_addr, 8);
    memcpy(buf + tl->off_pid, &probe_id, 8);

    if (!c->fsgsbase) {
        /* replace `rdfsbase %rax` (F3 0F AE C0) with `xor %rax,%rax; nop` */
        buf[tl->off_rdfsbase + 0] = 0x48;
        buf[tl->off_rdfsbase + 1] = 0x31;
        buf[tl->off_rdfsbase + 2] = 0xC0;
        buf[tl->off_rdfsbase + 3] = 0x90;
    }

    if (reloc_len > tl->reloc_size)
        reloc_len = tl->reloc_size;
    if (reloc_bytes && reloc_len > 0)
        memcpy(buf + tl->reloc_off, reloc_bytes, (size_t)reloc_len);

    int rc = mem_write(c, tramp_addr, buf, (size_t)total);
    free(buf);
    return rc;
}

int relocate_orig(const uint8_t *orig, int orig_len, uintptr_t orig_addr,
                  uintptr_t new_addr, uint8_t *out, int out_cap)
{
    if (orig_len > out_cap)
        return -1;
    int pos = 0;
    while (pos < orig_len) {
        x86_insn insn;
        int l = x86_dec_one(orig + pos, orig_len - pos, &insn);
        if (l <= 0 || pos + l > orig_len)
            return -1;
        memcpy(out + pos, orig + pos, (size_t)l);

        if (insn.rip_off >= 0) {
            int32_t disp;
            memcpy(&disp, orig + pos + insn.rip_off, 4);
            uintptr_t target = orig_addr + pos + l + (int64_t)disp;
            int64_t nd = (int64_t)target - (int64_t)(new_addr + pos + l);
            if (nd < INT32_MIN || nd > INT32_MAX)
                return -1;
            int32_t v = (int32_t)nd;
            memcpy(out + pos + insn.rip_off, &v, 4);
        }
        if (insn.rel_off >= 0) {
            uintptr_t target = orig_addr + pos + l;
            if (insn.rel_size == 1) {
                int8_t r8;
                memcpy(&r8, orig + pos + insn.rel_off, 1);
                target += (int8_t)r8;
            } else {
                int32_t r32;
                memcpy(&r32, orig + pos + insn.rel_off, 4);
                target += (int64_t)r32;
            }
            int64_t nd = (int64_t)target - (int64_t)(new_addr + pos + l);
            if (insn.rel_size == 1) {
                if (nd < -128 || nd > 127)
                    return -1;
                int8_t v = (int8_t)nd;
                memcpy(out + pos + insn.rel_off, &v, 1);
            } else {
                if (nd < INT32_MIN || nd > INT32_MAX)
                    return -1;
                int32_t v = (int32_t)nd;
                memcpy(out + pos + insn.rel_off, &v, 4);
            }
        }
        pos += l;
    }
    return pos;
}

/* ============================ events ============================ */

/* Drain committed events (ready-flag protocol, no producer-side commit spin).
 * Bulk-reads ready flags, finds the consecutive-ready prefix, then bulk-reads
 * the matching event payloads -- two preads per ring segment, not per event.
 *
 * If a producer ever laps us (consumer > cap behind), the ready flag holds a
 * sequence number *ahead* of what we expect. We skip straight to that sequence
 * instead of stalling: without this recovery the consumer would wait forever
 * on an already-overwritten slot. */
int events_read(ludp_ctx *c, ludp_event *out, int max, int *n)
{
    enum { FLAG_CHUNK = 8192 };
    u64 flags[FLAG_CHUNK];
    int count = 0;
    u64 consumed = c->consumed;

    while (count < max) {
        u64 idx = consumed & c->ring_mask;
        u64 to_end = c->ring_cap - idx;
        u64 want = (u64)(max - count);
        u64 chunk = want < to_end ? want : to_end;
        if (chunk > FLAG_CHUNK)
            chunk = FLAG_CHUNK;
        if (chunk == 0)
            break;

        /* Bulk-read ready flags for this contiguous ring segment. */
        uintptr_t fa = c->ring_addr + LUDP_READY_OFF + idx * 8;
        if (mem_read(c, fa, flags, (size_t)chunk * 8) != 0)
            break;

        /* How many leading slots are published (ready == logical seq + 1)? */
        u64 ready = 0;
        while (ready < chunk && flags[ready] == consumed + ready + 1)
            ready++;

        if (ready == 0) {
            u64 v = flags[0];
            if (v < consumed + 1)
                break;               /* not published yet (or stale wrap)   */
            /* v > consumed+1: a producer lapped us. Jump to the newest slot. */
            consumed = v - 1;
            continue;
        }

        /* Bulk-read the matching event payloads. */
        uintptr_t ea = c->ring_addr + c->events_off + idx * LUDP_EVENT_SZ;
        if (mem_read(c, ea, out + count, (size_t)ready * LUDP_EVENT_SZ) != 0)
            break;

        consumed += ready;
        count += (int)ready;
    }

    c->consumed = consumed;

    /* Publish our watermark so producers can compute overflow accurately. */
    mem_write(c, c->ring_addr + RO_COMMITTED, &consumed, 8);

    if (n)
        *n = count;
    return 0;
}

/* ============================ lifecycle ============================ */

static bool cpu_has_fsgsbase(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "flags") && strstr(line, "fsgsbase")) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

ludp_ctx *ludp_attach(pid_t pid)
{
    ludp_ctx *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->pid = pid;
    c->memfd = -1;

    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/mem", pid);
    c->memfd = open(mp, O_RDWR);
    if (c->memfd < 0) {
        free(c);
        return NULL;
    }
    c->fsgsbase = cpu_has_fsgsbase();

    if (ptrace(PTRACE_SEIZE, pid, 0, PTRACE_OPTS) != 0) {
        close(c->memfd);
        free(c);
        return NULL;
    }
    add_thread(c, pid);
    c->seized[0] = true;

    if (threads_refresh(c) != 0)
        goto fail;
    if (quiesce(c) != 0)
        goto fail;

    c->syscall_insn = find_syscall_insn(c);
    if (!c->syscall_insn)
        goto fail;

    /* ring buffer: RW mapping + header */
    c->ring_cap = LUDP_RING_CAP_DEFAULT;
    c->ring_mask = c->ring_cap - 1;
    c->events_off = LUDP_READY_OFF + (u64)c->ring_cap * 8;  /* after ready flags */
    size_t ring_bytes = c->events_off + (size_t)c->ring_cap * LUDP_EVENT_SZ;
    if (remote_mmap(c, ring_bytes, PROT_READ | PROT_WRITE, &c->ring_addr) != 0)
        goto fail;

    u64 zero = 0;
    mem_write(c, c->ring_addr + RO_SEQ, &zero, 8);
    mem_write(c, c->ring_addr + RO_COMMITTED, &zero, 8);
    mem_write(c, c->ring_addr + RO_CAP, &c->ring_cap, 8);
    mem_write(c, c->ring_addr + RO_OVERFLOW, &zero, 8);
    mem_write(c, c->ring_addr + RO_MASK, &c->ring_mask, 8);
    mem_write(c, c->ring_addr + RO_EVENTS_OFF, &c->events_off, 8);

    /* executable trampoline pool */
    c->exec_size = LUDP_EXEC_POOL_SIZE;
    if (remote_mmap(c, c->exec_size,
                    PROT_READ | PROT_WRITE | PROT_EXEC, &c->exec_base) != 0)
        goto fail;
    c->exec_next = c->exec_base;

    tramp_layout();
    c->attached = true;
    c->stopped = true;   /* still quiesced; caller resumes when ready */
    c->next_id = 1;
    return c;

fail:
    if (c->stopped)
        resume(c);
    for (int i = 0; i < c->nthreads; i++)
        if (c->seized[i])
            ptrace(PTRACE_DETACH, c->tids[i], 0, 0);
    if (c->memfd >= 0)
        close(c->memfd);
    free(c->tids);
    free(c->seized);
    free(c);
    return NULL;
}

void ludp_detach(ludp_ctx *c)
{
    if (!c)
        return;
    if (!c->attached) {
        free(c->tids); free(c->seized); free(c);
        return;
    }
    quiesce(c);

    /* remove every probe (restores original instruction bytes) */
    for (int i = c->nprobes - 1; i >= 0; i--) {
        if (c->probes[i].used)
            probe_remove_internal(c, c->probes[i].id);
    }

    for (int i = 0; i < c->nthreads; i++) {
        if (c->seized[i])
            ptrace(PTRACE_DETACH, c->tids[i], 0, 0);
    }
    if (c->memfd >= 0)
        close(c->memfd);
    free(c->tids);
    free(c->seized);
    free(c);
}

/* ============================ public wrappers ============================ */

int ludp_resolve(ludp_ctx *c, const char *lib, const char *sym, uintptr_t *addr)
{
    return elf_resolve(c->pid, lib, sym, addr, NULL, 0);
}

int ludp_probe_add(ludp_ctx *c, const char *lib, const char *sym, u64 *id_out)
{
    uintptr_t addr;
    if (elf_resolve(c->pid, lib, sym, &addr, NULL, 0) != 0)
        return -1;
    return probe_add_internal(c, sym, (lib && lib[0]) ? lib : "", addr, id_out);
}

int ludp_probe_add_addr(ludp_ctx *c, const char *sym, uintptr_t addr, u64 *id_out)
{
    return probe_add_internal(c, sym, "", addr, id_out);
}

int ludp_probe_remove(ludp_ctx *c, u64 id)
{
    return probe_remove_internal(c, id);
}

int ludp_probe_enable(ludp_ctx *c, u64 id)
{
    return probe_set_enabled(c, id, true);
}

int ludp_probe_disable(ludp_ctx *c, u64 id)
{
    return probe_set_enabled(c, id, false);
}

int ludp_probe_count(ludp_ctx *c)
{
    int n = 0;
    for (int i = 0; i < c->nprobes; i++)
        if (c->probes[i].used)
            n++;
    return n;
}

int ludp_probe_list(ludp_ctx *c, ludp_probe_info *out, int max)
{
    int n = 0;
    for (int i = 0; i < c->nprobes && n < max; i++) {
        probe_t *p = &c->probes[i];
        if (!p->used)
            continue;
        out[n].id = p->id;
        snprintf(out[n].sym, sizeof out[n].sym, "%s", p->sym);
        snprintf(out[n].lib, sizeof out[n].lib, "%s", p->lib);
        out[n].addr = p->addr;
        out[n].trampoline = p->tramp;
        out[n].orig_len = p->orig_len;
        out[n].enabled = p->enabled;
        out[n].hits = p->hits;
        n++;
    }
    return n;
}

int ludp_events_read(ludp_ctx *c, ludp_event *out, int max, int *n)
{
    return events_read(c, out, max, n);
}

u64 ludp_overflow(ludp_ctx *c)
{
    return mem_read_u64(c, c->ring_addr + RO_OVERFLOW);
}

u64 ludp_committed(ludp_ctx *c)
{
    return mem_read_u64(c, c->ring_addr + RO_COMMITTED);
}

int ludp_quiesce(ludp_ctx *c)
{
    return quiesce(c);
}

int ludp_resume(ludp_ctx *c)
{
    return resume(c);
}

int ludp_thread_count(ludp_ctx *c)
{
    return c->nthreads;
}

int ludp_mem_read(ludp_ctx *c, uintptr_t addr, void *buf, size_t len)
{
    return mem_read(c, addr, buf, len);
}

int ludp_mem_write(ludp_ctx *c, uintptr_t addr, const void *buf, size_t len)
{
    return mem_write(c, addr, buf, len);
}

u64 rdtsc_now(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}
