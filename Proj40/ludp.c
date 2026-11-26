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
#include <sys/uio.h>
#include <sys/syscall.h>
#include <signal.h>
#include <sys/auxv.h>
#include <elf.h>
#include "ludp_internal.h"
#if !LUDP_ARM64
#include <asm/hwcap2.h>
#endif

#define PTRACE_OPTS (PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL | PTRACE_O_TRACESYSGOOD)

/* trampoline template symbols (copied verbatim into the tracee) */
extern const char ludp_tramp_start[], ludp_tramp_end[];
extern const char ludp_tramp_code_end[];
extern const char ludp_tramp_data_begin[], ludp_tramp_data_end[];
extern const char ludp_tramp_reloc_begin[], ludp_tramp_reloc_end[];
extern const char ludp_tramp_ret_addr[], ludp_tramp_ring_ptr[];
extern const char ludp_tramp_probe_id[], ludp_tramp_rdfsbase[];
extern const char ludp_tramp_return_rdfsbase[], ludp_tramp_slots[], ludp_tramp_flags[];
extern const char ludp_return_stub[], ludp_return_stub_end[];
_Static_assert(sizeof(ludp_event) == LUDP_EVENT_SZ, "event ABI");
_Static_assert(offsetof(ludp_event, type) == 80, "event type offset");
_Static_assert(offsetof(ludp_event, xmm0) == 112, "FP return offset");

/* ============================ memory access ============================ */

static bool bulk_io_enabled(void)
{
    const char *mode = getenv("LUDP_BULK_IO");
    return !mode || strcmp(mode, "0") != 0;
}


int mem_read(ludp_ctx *c, uintptr_t addr, void *buf, size_t len)
{
    if (len == 0)
        return 0;
    /* 大块缓冲区优先单次跨进程复制；不可用时仍支持 /proc/pid/mem。 */
    if (len >= 16384 && bulk_io_enabled()) {
        struct iovec local = { .iov_base = buf, .iov_len = len };
        struct iovec remote = { .iov_base = (void *)addr, .iov_len = len };
        if (process_vm_readv(c->pid, &local, 1, &remote, 1, 0) == (ssize_t)len)
            return 0;
    }
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
    /* return stub 初始化为 1 MiB 大块写入，减少跨进程内存复制开销。 */
    if (len >= 16384 && bulk_io_enabled()) {
        struct iovec local = { .iov_base = (void *)buf, .iov_len = len };
        struct iovec remote = { .iov_base = (void *)addr, .iov_len = len };
        if (process_vm_writev(c->pid, &local, 1, &remote, 1, 0) == (ssize_t)len)
            return 0;
    }
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

int mem_write_code(ludp_ctx *c, uintptr_t addr, const void *buf, size_t len)
{
#if LUDP_ARM64
    /* /proc/pid/mem uses copy_to_user_page(), which synchronizes the
     * instruction cache for executable mappings. process_vm_writev does not. */
    ssize_t n = pwrite(c->memfd, buf, len, (off_t)addr);
    if (n == (ssize_t)len) return 0;
    if (n >= 0) errno = EIO;
    return -1;
#else
    return mem_write(c, addr, buf, len);
#endif
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
        /* 两个数组必须一起扩容：单独 realloc 后失败会丢失旧指针。 */
        int nc = c->cap_threads ? c->cap_threads * 2 : 32;
        pid_t *new_tids = malloc((size_t)nc * sizeof(*new_tids));
        bool *new_seized = malloc((size_t)nc * sizeof(*new_seized));
        if (!new_tids || !new_seized) {
            free(new_tids);
            free(new_seized);
            errno = ENOMEM;
            return -1;
        }
        if (c->nthreads) {
            memcpy(new_tids, c->tids, (size_t)c->nthreads * sizeof(*new_tids));
            memcpy(new_seized, c->seized, (size_t)c->nthreads * sizeof(*new_seized));
        }
        free(c->tids);
        free(c->seized);
        c->tids = new_tids;
        c->seized = new_seized;
        c->cap_threads = nc;
    }
    c->tids[c->nthreads] = tid;
    c->seized[c->nthreads] = false;
    return c->nthreads++;
}

/* 线程号与 ptrace 附加状态构成一个不可拆开的二元组。 */
static void sort_threads(ludp_ctx *c)
{
    for (int i = 1; i < c->nthreads; ++i) {
        int j = i;
        while (j > 0 && c->tids[j - 1] > c->tids[j]) {
            pid_t tid = c->tids[j - 1];
            c->tids[j - 1] = c->tids[j];
            c->tids[j] = tid;
            bool seized = c->seized[j - 1];
            c->seized[j - 1] = c->seized[j];
            c->seized[j] = seized;
            --j;
        }
    }
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
            remove_thread(c, c->tids[i]);
        }
    }

    /* Add any threads we have not seen before. */
    for (int k = 0; k < ncur; k++) {
        if (add_thread(c, cur[k]) < 0)
            return -1;
    }

    sort_threads(c);

    /* keep the leader (tgid) first so remote syscalls run on it */
    int li = tid_index(c, c->pid);
    if (li > 0) {
        pid_t t = c->tids[0]; c->tids[0] = c->tids[li]; c->tids[li] = t;
        bool s = c->seized[0]; c->seized[0] = c->seized[li]; c->seized[li] = s;
    }

    for (int i = 0; i < c->nthreads; i++) {
        if (!c->seized[i]) {
            if (ptrace(PTRACE_SEIZE, c->tids[i], 0, PTRACE_OPTS) == 0) {
                c->seized[i] = true;
            } else if (errno == ESRCH) {
                /* /proc 快照之后线程退出，下一次刷新会移除它。 */
                continue;
            } else {
                return -1;  /* 不得对尚未附加的线程修改代码 */
            }
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
    if (threads_refresh(c) != 0)
        return -1;

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
#if LUDP_ARM64
    return arm64_find_svc(c);
#else
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
#endif
}

static int remote_syscall(ludp_ctx *c, long nr, unsigned long a1, unsigned long a2,
                          unsigned long a3, unsigned long a4, unsigned long a5,
                          unsigned long a6, uintptr_t *ret)
{
#if LUDP_ARM64
    const unsigned long args[6] = { a1, a2, a3, a4, a5, a6 };
    return arm64_syscall(c, nr, args, ret);
#else
    pid_t tid = -1;
    struct user_regs_struct orig, regs;

    /*
     * 不在被内核中断的阻塞系统调用上劫持执行流：这会破坏 futex
     * 等系统调用的 ERESTARTSYS 语义，导致应用在 pthread_join 中报错。
     * 只选择已经 ptrace 停止、且当前在用户代码中的线程执行远程 mmap。
     * 找不到安全线程时拒绝插入，而不是悄悄损坏原进程上下文。
     */
    for (int i = 0; i < c->nthreads; ++i) {
        if (!c->seized[i]) continue;
        if (ptrace(PTRACE_GETREGS, c->tids[i], 0, &regs) != 0) continue;
        if (regs.orig_rax == (unsigned long)-1) {
            tid = c->tids[i];
            orig = regs;
            break;
        }
    }
    if (tid < 0) { errno = EBUSY; return -1; }
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
#endif
}

/* All entries are mappings allocated by this context. A failed release
 * stays on the list so teardown can retry without touching other mappings. */
static void record_mapping(ludp_ctx *c, owned_mapping *m, uintptr_t base,
                           size_t size, int prot)
{
    *m = (owned_mapping){base, size, (prot & PROT_EXEC) != 0, c->mappings};
    c->mappings = m;
}

int release_idle_storage(ludp_ctx *c, bool closing)
{
    while (c->retired) {
        retired_slot *slot = c->retired;
        c->retired = slot->next;
        free(slot);
    }
    owned_mapping **link = &c->mappings;
    while (*link) {
        owned_mapping *m = *link;
        if (!closing && (m->base == c->exec_base || m->base == c->ring_addr)) {
            link = &m->next;
            continue;
        }
        uintptr_t result;
        if (remote_syscall(c, __NR_munmap, m->base, m->size, 0, 0, 0, 0, &result))
            return -1;
        if (result) { errno = -(long)result; return -1; }
        if (m->base == c->return_slots) {
            c->return_slots = 0;
            c->slots_ready = false;
        }
        if (m->base == c->far_base) c->far_valid = false;
        if (m->base == c->exec_base) c->exec_base = 0;
        if (m->base == c->ring_addr) c->ring_addr = 0;
        *link = m->next;
        free(m);
    }
    c->exec_next = c->exec_base;
    return 0;
}

/* The caller also coordinates a safe point with the application: a signal
 * handler could otherwise conceal a trampoline PC in a saved signal frame. */
int probe_storage_idle(ludp_ctx *c)
{
    bool has_user_thread = false;
    for (int i = 0; i < c->nthreads; ++i) {
        if (!c->seized[i]) continue;
        uintptr_t pc;
#if LUDP_ARM64
        bool in_syscall;
        if (arm64_thread_state(c->tids[i], &pc, &in_syscall)) return -1;
        if (!in_syscall) has_user_thread = true;
#else
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, c->tids[i], 0, &regs)) return -1;
        if (regs.orig_rax == (unsigned long)-1) has_user_thread = true;
        pc = regs.rip;
#endif
        for (owned_mapping *m = c->mappings; m; m = m->next) {
            if (m->executable && pc >= m->base && pc - m->base < m->size) {
                errno = EBUSY;
                return -1;
            }
        }
    }
    if (c->mappings && !has_user_thread) {
        errno = EBUSY;
        return -1;
    }
    if (c->slots_ready) {
        /* Byte 64 is the owner word in trampoline.S. Read 64 slots at once. */
        u64 batch[64 * LUDP_RETURN_SLOT_SZ / sizeof(u64)];
        for (int i = 0; i < LUDP_RETURN_SLOTS; i += 64) {
            if (mem_read(c, c->return_slots + i * LUDP_RETURN_SLOT_SZ, batch, sizeof batch))
                return -1;
            for (int j = 0; j < 64; ++j) {
                if (batch[(j * LUDP_RETURN_SLOT_SZ + 64) / sizeof(u64)]) {
                    errno = EBUSY;
                    return -1;
                }
            }
        }
    }
    return 0;
}

int remote_mmap(ludp_ctx *c, size_t size, int prot, uintptr_t *out)
{
    uintptr_t ret = 0;
    owned_mapping *m = malloc(sizeof *m);
    if (!m) return -1;
    int rc = remote_syscall(c, __NR_mmap, 0, size, prot,
                            MAP_PRIVATE | MAP_ANONYMOUS,
                            (unsigned long)-1, 0, &ret);
    if (rc || (long)ret < 0) { free(m); return -1; }
    record_mapping(c, m, ret, size, prot);
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
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return -1;
    uintptr_t page_mask = (uintptr_t)page_size - 1;
    size = (size + page_mask) & ~page_mask;
    if (!size) size = page_size;

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

    const uintptr_t LIMIT = LUDP_ARM64 ? 0x03c00000UL : 0x78000000UL;
    const uintptr_t STEP = 0x100000UL;      /* 1 MB scan granularity */

    owned_mapping *m = malloc(sizeof *m);
    if (!m) return -1;
    for (int dir = 0; dir < 2; dir++) {
        for (uintptr_t off = 0; off <= LIMIT; off += STEP) {
            int64_t cand = dir == 0 ? (int64_t)target + (int64_t)off
                                    : (int64_t)target - (int64_t)off;
            if (cand <= 0x10000)
                continue;
            uintptr_t p = ((uintptr_t)cand + page_mask) & ~page_mask;
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
            if (rc == 0 && (long)ret >= 0) {
                record_mapping(c, m, ret, size, prot);
                if (ret != p) { errno = ENOTSUP; return -1; }
                if (out)
                    *out = p;
                return 0;
            }
        }
    }
    free(m);
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
#if !LUDP_ARM64
        l.off_rdfsbase = (int)(ludp_tramp_rdfsbase - ludp_tramp_start);
        l.off_return_rdfsbase = (int)(ludp_tramp_return_rdfsbase - ludp_tramp_start);
#endif
        l.off_slots = (int)(ludp_tramp_slots - ludp_tramp_start);
        l.off_flags = (int)(ludp_tramp_flags - ludp_tramp_start);
        init = 1;
    }
    return &l;
}

int tramp_build(ludp_ctx *c, uintptr_t tramp_addr, u64 probe_id,
                uintptr_t ring_addr, uintptr_t ret_addr,
                const uint8_t *reloc_bytes, int reloc_len, unsigned flags)
{
    const tramp_layout_t *tl = tramp_layout();
    int total = tl->total_size;
    uint8_t *buf = malloc((size_t)total);
    if (!buf)
        return -1;

    memcpy(buf, ludp_tramp_start, (size_t)total);

#if LUDP_ARM64
    (void)ret_addr; /* Linked as a direct branch before the entry is patched. */
#else
    memcpy(buf + tl->off_ret, &ret_addr, 8);
#endif
    memcpy(buf + tl->off_ring, &ring_addr, 8);
    memcpy(buf + tl->off_pid, &probe_id, 8);

    u64 f = flags;
    memcpy(buf + tl->off_slots, &c->return_slots, 8);
    memcpy(buf + tl->off_flags, &f, 8);

#if !LUDP_ARM64
    if (!c->fsgsbase) {
        /* rdfsbase %rax is FIVE bytes: F3 48 0F AE C0. */
        const uint8_t fallback[5] = {0x31, 0xc0, 0x90, 0x90, 0x90};
        memcpy(buf + tl->off_rdfsbase, fallback, sizeof fallback);
        memcpy(buf + tl->off_return_rdfsbase, fallback, sizeof fallback);
    }

#endif

    if (reloc_len > tl->reloc_size)
        reloc_len = tl->reloc_size;
    if (reloc_bytes && reloc_len > 0)
        memcpy(buf + tl->reloc_off, reloc_bytes, (size_t)reloc_len);

    int rc = mem_write_code(c, tramp_addr, buf, (size_t)total);
    free(buf);
    return rc;
}

#if !LUDP_ARM64
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
            if (target >= orig_addr && target < orig_addr + orig_len)
                target = new_addr + (target - orig_addr);
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
#endif

/* ============================ events ============================ */

/* Drain the consecutive published prefix. Producers drop on full and cannot
 * reuse any of these payloads until our final watermark store. */
int events_read(ludp_ctx *c, ludp_event *out, int max, int *n)
{
    enum { FLAG_CHUNK = 8192 };
    u64 flags[FLAG_CHUNK];
    if (!c || !out || max < 0) return -1;
    if (n) *n = 0;
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
            return -1;

        /* How many leading slots are published (ready == logical seq + 1)? */
        u64 ready = 0;
        while (ready < chunk && flags[ready] == consumed + ready + 1)
            ready++;

        if (ready == 0) break;

#if LUDP_ARM64
        /* Observe the payload after the producer's release publication. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
        /* Bulk-read the matching event payloads. */
        uintptr_t ea = c->ring_addr + c->events_off + idx * LUDP_EVENT_SZ;
        if (mem_read(c, ea, out + count, (size_t)ready * LUDP_EVENT_SZ) != 0)
            return -1;

        for (u64 k = 0; k < ready; ++k) {
            u64 id = out[count + k].probe_id;
            /* IDs normally match allocation order. Avoid a registry scan per
             * event on the high-volume path; failed insertions may leave gaps. */
            int pi = id > 0 && id <= (u64)c->nprobes &&
                     c->probes[id - 1].used && c->probes[id - 1].id == id
                         ? (int)id - 1 : find_probe(c, id);
            if (pi >= 0 && out[count + k].type == LUDP_EVENT_ENTRY)
                c->probes[pi].hits++;
        }
        consumed += ready;
        count += (int)ready;
    }

#if LUDP_ARM64
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
    /* Publish our watermark so producers can compute overflow accurately. */
    if (mem_write(c, c->ring_addr + RO_COMMITTED, &consumed, 8) != 0) return -1;
    c->consumed = consumed;

    if (n)
        *n = count;
    return 0;
}

/* ============================ lifecycle ============================ */

static u64 configured_ring_cap(void)
{
    const char *v = getenv("LUDP_RING_CAP");
    if (!v || !*v)
        return LUDP_RING_CAP_DEFAULT;
    errno = 0;
    char *end = NULL;
    unsigned long long cap = strtoull(v, &end, 10);
    if (errno || end == v || *end || cap < 16 ||
        cap > LUDP_RING_CAP_DEFAULT || (cap & (cap - 1))) {
        errno = EINVAL;
        return 0;
    }
    return (u64)cap;
}

static bool cpu_has_fsgsbase(void)
{
    /* CPU support alone does not imply the kernel enabled CR4.FSGSBASE. */
#ifdef HWCAP2_FSGSBASE
    return (getauxval(AT_HWCAP2) & HWCAP2_FSGSBASE) != 0;
#else
    return false; /* Older Linux headers: use the existing TLS fallback. */
#endif
}

int return_slots_init(ludp_ctx *c)
{
    if (c->slots_ready) return 0;
#if LUDP_ARM64
    if (arm64_check_return_abi(c)) return -1;
#else
    /* Return-address substitution is incompatible with an active CET SHSTK. */
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/status", c->pid);
    FILE *status = fopen(path, "r");
    if (!status) return -1;
    while (fgets(line, sizeof line, status)) {
        if (!strncmp(line, "x86_Thread_features:", 20) && strstr(line, "shstk")) {
            fclose(status); errno = ENOTSUP; return -1;
        }
    }
    fclose(status);
#endif
    size_t size = LUDP_RETURN_SLOTS * LUDP_RETURN_SLOT_SZ;
    uint8_t *buf = calloc(1, size);
    if (!buf) return -1;
    size_t stub_size = ludp_return_stub_end - ludp_return_stub;
    if (stub_size != 64) { free(buf); return -1; }
    for (int i = 0; i < LUDP_RETURN_SLOTS; ++i)
        memcpy(buf + i * LUDP_RETURN_SLOT_SZ, ludp_return_stub, stub_size);
    int rc = 0;
    if (!c->return_slots)
        rc = remote_mmap(c, size, PROT_READ | PROT_WRITE | PROT_EXEC, &c->return_slots);
    if (!rc) rc = mem_write_code(c, c->return_slots, buf, size);
    c->slots_ready = rc == 0;
    free(buf);
    return rc;
}

static int check_tracee_arch(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/exe", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    Elf64_Ehdr hdr;
    ssize_t n = read(fd, &hdr, sizeof hdr);
    int saved = errno;
    close(fd);
    if (n < 0) { errno = saved; return -1; }
    if (n != sizeof hdr || memcmp(hdr.e_ident, ELFMAG, SELFMAG) ||
        hdr.e_ident[EI_CLASS] != ELFCLASS64 || hdr.e_ident[EI_DATA] != ELFDATA2LSB ||
        hdr.e_machine != (LUDP_ARM64 ? EM_AARCH64 : EM_X86_64)) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

ludp_ctx *ludp_attach(pid_t pid)
{
    ludp_ctx *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->pid = pid;
    c->memfd = -1;

    c->fsgsbase = cpu_has_fsgsbase();

    if (ptrace(PTRACE_SEIZE, pid, 0, PTRACE_OPTS) != 0) {
        free(c);
        return NULL;
    }
    if (add_thread(c, pid) < 0) {
        ptrace(PTRACE_INTERRUPT, pid, 0, 0);
        if (wait_stop(pid, 3000) >= 0) ptrace(PTRACE_DETACH, pid, 0, 0);
        goto fail;
    }
    c->seized[0] = true;

    if (threads_refresh(c) != 0)
        goto fail;
    if (quiesce(c) != 0)
        goto fail;

    if (check_tracee_arch(pid)) goto fail;
    char mp[64];
    snprintf(mp, sizeof mp, "/proc/%d/mem", pid);
    c->memfd = open(mp, O_RDWR | O_CLOEXEC);
    if (c->memfd < 0) goto fail;

    c->syscall_insn = find_syscall_insn(c);
    if (!c->syscall_insn)
        goto fail;

    /* ring buffer: RW mapping + header */
    c->ring_cap = configured_ring_cap();
    if (!c->ring_cap)
        goto fail;
    c->ring_mask = c->ring_cap - 1;
    c->events_off = LUDP_READY_OFF + (u64)c->ring_cap * 8;  /* after ready flags */
    size_t ring_bytes = c->events_off + (size_t)c->ring_cap * LUDP_EVENT_SZ;
    if (remote_mmap(c, ring_bytes, PROT_READ | PROT_WRITE, &c->ring_addr) != 0)
        goto fail;

    u64 zero = 0;
    if (mem_write(c, c->ring_addr + RO_SEQ, &zero, 8) ||
        mem_write(c, c->ring_addr + RO_COMMITTED, &zero, 8) ||
        mem_write(c, c->ring_addr + RO_CAP, &c->ring_cap, 8) ||
        mem_write(c, c->ring_addr + RO_OVERFLOW, &zero, 8) ||
        mem_write(c, c->ring_addr + RO_MASK, &c->ring_mask, 8) ||
        mem_write(c, c->ring_addr + RO_EVENTS_OFF, &c->events_off, 8))
        goto fail;

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
    if (c->mappings) release_idle_storage(c, true);
    for (int i = 0; i < c->nthreads; i++)
        if (c->seized[i])
            ptrace(PTRACE_DETACH, c->tids[i], 0, 0);
    if (c->memfd >= 0)
        close(c->memfd);
    free(c->tids);
    free(c->seized);
    while (c->mappings) {
        owned_mapping *next = c->mappings->next;
        free(c->mappings);
        c->mappings = next;
    }
    free(c);
    return NULL;
}

int ludp_detach(ludp_ctx *c)
{
    if (!c) return 0;
    bool running = !c->stopped;
    if (quiesce(c)) return -1;
    if (!c->closing && probe_storage_idle(c)) {
        int err = errno;
        if (running) resume(c);
        errno = err;
        return -1;
    }
    c->closing = true;
    for (int i = c->nprobes - 1; i >= 0; --i)
        if (c->probes[i].used && probe_remove_internal(c, c->probes[i].id))
            return -1;

    if (release_idle_storage(c, true)) return -1;
    for (int i = 0; i < c->nthreads; ++i) {
        if (!c->seized[i]) continue;
        if (ptrace(PTRACE_DETACH, c->tids[i], 0, 0) && errno != ESRCH)
            return -1;
        c->seized[i] = false;
    }
    if (c->memfd >= 0)
        close(c->memfd);
    free(c->tids);
    free(c->seized);
    free(c);
    return 0;
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
    return probe_add_internal(c, sym, (lib && lib[0]) ? lib : "", addr, LUDP_PROBE_ENTRY, id_out);
}

int ludp_probe_add_addr(ludp_ctx *c, const char *sym, uintptr_t addr, u64 *id_out)
{
    return probe_add_internal(c, sym, "", addr, LUDP_PROBE_ENTRY, id_out);
}

int ludp_probe_add_ex(ludp_ctx *c, const char *lib, const char *sym, unsigned flags, u64 *id_out)
{
    uintptr_t addr;
    if (!c || !sym || elf_resolve(c->pid, lib, sym, &addr, NULL, 0)) return -1;
    return probe_add_internal(c, sym, lib ? lib : "", addr, flags, id_out);
}

int ludp_probe_add_addr_ex(ludp_ctx *c, const char *sym, uintptr_t addr, unsigned flags, u64 *id_out)
{
    return probe_add_internal(c, sym, "", addr, flags, id_out);
}

u64 ludp_return_missed(ludp_ctx *c)
{
    return mem_read_u64(c, c->ring_addr + 168);
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
#if LUDP_ARM64
    u64 count;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(count));
    return count;
#else
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
#endif
}
