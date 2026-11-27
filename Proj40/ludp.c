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
#include <time.h>
#include <sys/auxv.h>
#include <elf.h>
#if defined(__x86_64__)
#include <asm/hwcap2.h>
#include <cpuid.h>
#elif defined(__aarch64__)
#include <asm/ptrace.h>
#else
#error "LUDP supports Linux x86_64 and AArch64"
#endif
#include "ludp_internal.h"

#define PTRACE_OPTS (PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL | \
                     PTRACE_O_TRACESYSGOOD)
#define SEIZE_RETRY_COUNT 50

/* trampoline template symbols (copied verbatim into the tracee) */
extern const char ludp_tramp_start[], ludp_tramp_end[];
extern const char ludp_tramp_code_end[];
extern const char ludp_tramp_data_begin[], ludp_tramp_data_end[];
extern const char ludp_tramp_reloc_begin[], ludp_tramp_reloc_end[];
extern const char ludp_tramp_ret_addr[], ludp_tramp_ring_ptr[];
extern const char ludp_tramp_probe_id[], ludp_tramp_rdfsbase[];
extern const char ludp_tramp_return_rdfsbase[], ludp_tramp_slots[], ludp_tramp_flags[];
extern const char ludp_tramp_filter_op[], ludp_tramp_filter_arg[];
extern const char ludp_tramp_filter_value[], ludp_tramp_filter_mask[];
extern const char ludp_tramp_call_fn[], ludp_tramp_call_data[];
extern const char ludp_tramp_call_active[];
extern const char ludp_tramp_xsave_mode[], ludp_tramp_xsave_size[];
extern const char ludp_tramp_xsave_mask[];
extern const char ludp_return_stub[], ludp_return_stub_end[];
#if defined(__aarch64__)
extern const char ludp_tramp_exit[];
static int arm64_check_context(ludp_ctx *c);
#endif
_Static_assert(sizeof(ludp_raw_event) == LUDP_EVENT_SZ, "remote event ABI");
_Static_assert(offsetof(ludp_raw_event, type) == 80, "event type offset");
_Static_assert(offsetof(ludp_raw_event, xmm0) == 112, "FP return offset");
_Static_assert(offsetof(ludp_event, monotonic_ns) == LUDP_EVENT_SZ,
               "public event prefix must match remote ABI");
_Static_assert(sizeof(ludp_call_context) == 64, "A4 callback context ABI");
_Static_assert(offsetof(ludp_call_context, args) == 8, "A4 args offset");
_Static_assert(offsetof(ludp_call_context, user_data) == 56,
               "A4 user-data offset");

/* ============================ memory access ============================ */

static bool bulk_io_enabled(void)
{
    const char *mode = getenv("LUDP_BULK_IO");
    return !mode || strcmp(mode, "0") != 0;
}


int mem_read(ludp_ctx *c, uintptr_t addr, void *buf, size_t len)
{
    if (!c || c->memfd < 0 || (!buf && len) || addr > UINTPTR_MAX - len) {
        errno = EINVAL;
        return -1;
    }
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
    if (!c || c->memfd < 0 || (!buf && len) || addr > UINTPTR_MAX - len) {
        errno = EINVAL;
        return -1;
    }
    if (len == 0)
        return 0;
    /* ARM64 executable writes must go through access_remote_vm(), whose
     * copy_to_user_page() hook synchronizes the data and instruction caches.
     * /proc/pid/mem and ptrace both use this hook; process_vm_writev does not.
     * A single pwrite still copies the entire return-slot arena efficiently. */
#if defined(__x86_64__)
    if (len >= 16384 && bulk_io_enabled()) {
        struct iovec local = { .iov_base = (void *)buf, .iov_len = len };
        struct iovec remote = { .iov_base = (void *)addr, .iov_len = len };
        if (process_vm_writev(c->pid, &local, 1, &remote, 1, 0) == (ssize_t)len)
            return 0;
    }
#endif
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
    if (mem_read(c, addr, &v, sizeof v) != 0)
        return 0;
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

/* PTRACE_SEIZE may transiently report EBUSY for a freshly forked task even
 * though /proc already exposes it and no tracer owns it.  Retrying blindly is
 * unsafe: an actual debugger must never be displaced.  Use TracerPid to
 * distinguish the short kernel hand-off window from a real ownership clash.
 * A thread already owned by this process can be observed after TRACECLONE and
 * is considered successfully seized only while refreshing an existing
 * context; an initial attach still rejects duplicate ownership. */
static int read_tracer_pid(pid_t tid, pid_t *tracer_out)
{
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/status", tid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof line, f)) {
        long tracer;
        if (sscanf(line, "TracerPid:%ld", &tracer) == 1) {
            fclose(f);
            *tracer_out = (pid_t)tracer;
            return 0;
        }
    }
    fclose(f);
    errno = EPROTO;
    return -1;
}

static int seize_thread(pid_t tid, bool accept_owned)
{
    for (int attempt = 0; attempt < SEIZE_RETRY_COUNT; ++attempt) {
        if (ptrace(PTRACE_SEIZE, tid, 0, PTRACE_OPTS) == 0)
            return 0;

        int err = errno;
        if (err != EBUSY) {
            errno = err;
            return -1;
        }

        pid_t tracer = 0;
        if (read_tracer_pid(tid, &tracer) != 0)
            return -1;
        if (accept_owned && tracer == getpid())
            return 0;
        if (tracer != 0) {
            errno = EBUSY;
            return -1;
        }

        if (attempt + 1 < SEIZE_RETRY_COUNT)
            usleep(1000);
    }
    errno = EBUSY;
    return -1;
}

static void remove_thread(ludp_ctx *c, pid_t tid)
{
    int i = tid_index(c, tid);
    if (i < 0)
        return;
    c->tids[i] = c->tids[c->nthreads - 1];
    c->seized[i] = c->seized[c->nthreads - 1];
    c->thread_stopped[i] = c->thread_stopped[c->nthreads - 1];
    c->pending_sigs[i] = c->pending_sigs[c->nthreads - 1];
    c->fsbases[i] = c->fsbases[c->nthreads - 1];
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
        bool *new_thread_stopped = calloc((size_t)nc,
                                          sizeof(*new_thread_stopped));
        int *new_pending_sigs = calloc((size_t)nc, sizeof(*new_pending_sigs));
        u64 *new_fsbases = calloc((size_t)nc, sizeof(*new_fsbases));
        if (!new_tids || !new_seized || !new_thread_stopped ||
            !new_pending_sigs || !new_fsbases) {
            free(new_tids);
            free(new_seized);
            free(new_thread_stopped);
            free(new_pending_sigs);
            free(new_fsbases);
            errno = ENOMEM;
            return -1;
        }
        if (c->nthreads) {
            memcpy(new_tids, c->tids, (size_t)c->nthreads * sizeof(*new_tids));
            memcpy(new_seized, c->seized, (size_t)c->nthreads * sizeof(*new_seized));
            memcpy(new_thread_stopped, c->thread_stopped,
                   (size_t)c->nthreads * sizeof(*new_thread_stopped));
            memcpy(new_pending_sigs, c->pending_sigs,
                   (size_t)c->nthreads * sizeof(*new_pending_sigs));
            memcpy(new_fsbases, c->fsbases,
                   (size_t)c->nthreads * sizeof(*new_fsbases));
        }
        free(c->tids);
        free(c->seized);
        free(c->thread_stopped);
        free(c->pending_sigs);
        free(c->fsbases);
        c->tids = new_tids;
        c->seized = new_seized;
        c->thread_stopped = new_thread_stopped;
        c->pending_sigs = new_pending_sigs;
        c->fsbases = new_fsbases;
        c->cap_threads = nc;
    }
    c->tids[c->nthreads] = tid;
    c->seized[c->nthreads] = false;
    c->thread_stopped[c->nthreads] = false;
    c->pending_sigs[c->nthreads] = 0;
    c->fsbases[c->nthreads] = 0;
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
            bool stopped = c->thread_stopped[j - 1];
            c->thread_stopped[j - 1] = c->thread_stopped[j];
            c->thread_stopped[j] = stopped;
            int pending_sig = c->pending_sigs[j - 1];
            c->pending_sigs[j - 1] = c->pending_sigs[j];
            c->pending_sigs[j] = pending_sig;
            u64 fsbase = c->fsbases[j - 1];
            c->fsbases[j - 1] = c->fsbases[j];
            c->fsbases[j] = fsbase;
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
    pid_t *cur = NULL;
    int ncur = 0, cap_cur = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char *end = NULL;
        long value = strtol(e->d_name, &end, 10);
        if (!end || *end || value <= 0)
            continue;
        if (ncur == cap_cur) {
            int next = cap_cur ? cap_cur * 2 : 64;
            pid_t *grown = realloc(cur, (size_t)next * sizeof(*grown));
            if (!grown) {
                int err = errno;
                free(cur);
                closedir(d);
                errno = err;
                return -1;
            }
            cur = grown;
            cap_cur = next;
        }
        cur[ncur++] = (pid_t)value;
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
        if (add_thread(c, cur[k]) < 0) {
            free(cur);
            return -1;
        }
    }
    free(cur);

    sort_threads(c);

    /* keep the leader (tgid) first so remote syscalls run on it */
    int li = tid_index(c, c->pid);
    if (li > 0) {
        pid_t t = c->tids[0]; c->tids[0] = c->tids[li]; c->tids[li] = t;
        bool s = c->seized[0]; c->seized[0] = c->seized[li]; c->seized[li] = s;
        bool stopped = c->thread_stopped[0];
        c->thread_stopped[0] = c->thread_stopped[li];
        c->thread_stopped[li] = stopped;
        int sig = c->pending_sigs[0];
        c->pending_sigs[0] = c->pending_sigs[li];
        c->pending_sigs[li] = sig;
        u64 fsbase = c->fsbases[0];
        c->fsbases[0] = c->fsbases[li];
        c->fsbases[li] = fsbase;
    }

    for (int i = 0; i < c->nthreads; i++) {
        if (!c->seized[i]) {
            if (seize_thread(c->tids[i], true) == 0) {
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

static void update_thread_fsbase(ludp_ctx *c, pid_t tid)
{
    int i = tid_index(c, tid);
    if (i < 0)
        return;
#if defined(__aarch64__)
    u64 tls;
    struct iovec io = { .iov_base = &tls, .iov_len = sizeof tls };
    if (ptrace(PTRACE_GETREGSET, tid, (void *)NT_ARM_TLS, &io) == 0 &&
        io.iov_len == sizeof tls)
        c->fsbases[i] = tls;
#else
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, tid, 0, &regs) == 0)
        c->fsbases[i] = (u64)regs.fs_base;
#endif
}

static void refresh_thread_fsbases(ludp_ctx *c)
{
    for (int i = 0; i < c->nthreads; ++i)
        if (c->seized[i])
            update_thread_fsbase(c, c->tids[i]);
}

typedef struct {
    struct timespec deadline;
    unsigned polls;
} stop_wait;

static int stop_wait_init(stop_wait *wait, int timeout_ms)
{
    if (clock_gettime(CLOCK_MONOTONIC, &wait->deadline)) return -1;
    wait->deadline.tv_sec += timeout_ms / 1000;
    wait->deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (wait->deadline.tv_nsec >= 1000000000L) {
        ++wait->deadline.tv_sec;
        wait->deadline.tv_nsec -= 1000000000L;
    }
    wait->polls = 0;
    return 0;
}

static int64_t stop_wait_remaining(const stop_wait *wait)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    int64_t remaining = (int64_t)(wait->deadline.tv_sec - now.tv_sec) *
                        INT64_C(1000000000) +
                        wait->deadline.tv_nsec - now.tv_nsec;
    if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
    return remaining;
}

/* Most ptrace stops arrive within a few microseconds. A fixed 1 ms sleep
 * after every empty waitpid artificially adds milliseconds to every remote
 * mmap/munmap. Briefly poll, then back off; the monotonic deadline still
 * bounds the entire wait, including scheduling delays and EINTR retries. */
static int stop_wait_pause(stop_wait *wait)
{
    int64_t remaining = stop_wait_remaining(wait);
    if (remaining < 0) return -1;
    /* A ptrace stop commonly lands just after the old 16-poll window.  Keep
     * the control thread busy for a few more cheap nonblocking probes before
     * paying a 50 us scheduler sleep.  This path runs only for structural
     * operations, never for ordinary probe hits. */
    if (wait->polls++ < 64) return 0;
    long delay = wait->polls < 256 ? 50000L : 1000000L;
    if (remaining < delay) delay = (long)remaining;
    struct timespec pause = { .tv_nsec = delay };
    if (nanosleep(&pause, NULL) && errno != EINTR) return -1;
    return 0;
}

int wait_stop(pid_t tid, int timeout_ms)
{
    stop_wait wait;
    if (stop_wait_init(&wait, timeout_ms)) return -1;
    for (;;) {
        int st;
        pid_t w = waitpid(tid, &st, __WALL | WNOHANG);
        if (w == tid) return st;
        if (w < 0 && errno != EINTR) return -1;
        if (stop_wait_pause(&wait)) return -1;
    }
}

static bool all_threads_stopped(const ludp_ctx *c)
{
    bool any = false;
    for (int i = 0; i < c->nthreads; ++i) {
        if (!c->seized[i])
            continue;
        any = true;
        if (!c->thread_stopped[i])
            return false;
    }
    return any;
}

/* Resume only stops whose wait status has already been consumed.  Keeping
 * this state per tid makes a failed/partial resume recoverable by the next
 * quiesce instead of silently stranding one thread. */
static int resume_known_stops(ludp_ctx *c)
{
    if (c->unsafe_state) {
        errno = EIO;
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < c->nthreads;) {
        if (!c->seized[i] || !c->thread_stopped[i]) {
            ++i;
            continue;
        }
        pid_t tid = c->tids[i];
        int sig = c->pending_sigs[i];
        if (ptrace(PTRACE_CONT, tid, 0, (void *)(uintptr_t)sig) != 0) {
            if (errno == ESRCH) {
                remove_thread(c, tid);
                continue;
            }
            rc = -1;
            ++i;
            continue;
        }
        c->thread_stopped[i] = false;
        c->pending_sigs[i] = 0;
        ++i;
    }
    c->stopped = all_threads_stopped(c);
    return rc;
}

int quiesce(ludp_ctx *c)
{
    if (!c) {
        errno = EINVAL;
        return -1;
    }
    if (c->unsafe_state) {
        errno = EIO;
        return -1;
    }
    if (c->stopped && all_threads_stopped(c)) {
#if defined(__aarch64__)
        return arm64_check_context(c);
#else
        return 0;
#endif
    }
    c->stopped = false;
    if (threads_refresh(c) != 0)
        return -1;

    /* "pending" holds every live tid we are waiting to see stop (or exit).
     * Keeping it index-free avoids the pitfalls of stale array positions when
     * a thread exits and is removed mid-quiesce. */
    int pending_cap = c->nthreads > 0 ? c->nthreads : 1;
    pid_t *pending = malloc((size_t)pending_cap * sizeof(pid_t));
    if (!pending)
        return -1;
    int remain = 0;
    for (int i = 0; i < c->nthreads; i++) {
        if (!c->seized[i] || c->thread_stopped[i])
            continue;
        pending[remain++] = c->tids[i];
        if (ptrace(PTRACE_INTERRUPT, c->tids[i], 0, 0) != 0 &&
            errno != ESRCH && errno != EIO) {
            int err = errno;
            free(pending);
            resume_known_stops(c);
            errno = err;
            return -1;
        }
    }

    stop_wait wait;
    if (stop_wait_init(&wait, 3000)) {
        int err = errno;
        free(pending);
        resume_known_stops(c);
        errno = err;
        return -1;
    }

    int wait_error = 0;
    while (remain > 0) {
        /* A continuous stream of clone/exit/signal statuses must not bypass
         * the deadline simply because waitpid never reports an empty queue. */
        if (stop_wait_remaining(&wait) < 0) {
            wait_error = errno;
            break;
        }
        int st;
        pid_t w = waitpid(-1, &st, __WALL | WNOHANG);
        if (w > 0) {
            if (WIFSTOPPED(st)) {
                int wi = tid_index(c, w);
                if (wi >= 0)
                    c->thread_stopped[wi] = true;
                int ev = (st >> 16) & 0xffff;
                if (ev == PTRACE_EVENT_CLONE) {
                    unsigned long nt = 0;
                    if (ptrace(PTRACE_GETEVENTMSG, w, 0, &nt) == 0 && nt > 0) {
                        int ni = add_thread(c, (pid_t)nt);
                        if (ni >= 0) {
                            c->seized[ni] = true;
                            if (remain == pending_cap) {
                                int next_cap = pending_cap * 2;
                                pid_t *np = realloc(
                                    pending, (size_t)next_cap * sizeof(pid_t));
                                if (!np) {
                                    int err = errno;
                                    free(pending);
                                    resume_known_stops(c);
                                    errno = err;
                                    return -1;
                                }
                                pending = np;
                                pending_cap = next_cap;
                            }
                            bool tracked = false;
                            for (int k = 0; k < remain; ++k)
                                if (pending[k] == (pid_t)nt) tracked = true;
                            if (!tracked)
                                pending[remain++] = (pid_t)nt;
                        }
                    }
                }
                /* PTRACE_INTERRUPT and ptrace event stops have a non-zero
                 * event code.  A zero event code is a real signal-delivery
                 * stop; preserve it across the structural update. */
                if (ev == 0 && wi >= 0)
                    c->pending_sigs[wi] = WSTOPSIG(st);
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
        if (w < 0 && errno != EINTR) {
            int err = errno;
            free(pending);
            resume_known_stops(c);
            errno = err;
            return -1;
        }
        if (stop_wait_pause(&wait)) {
            wait_error = errno;
            break;
        }
    }
    free(pending);

    if (remain > 0) {
        int err = wait_error ? wait_error : ETIMEDOUT;
        resume_known_stops(c);
        errno = err;
        return -1;
    }
    refresh_thread_fsbases(c);
    c->stopped = all_threads_stopped(c);
    if (!c->stopped) {
        errno = EBUSY;
        return -1;
    }
#if defined(__aarch64__)
    if (arm64_check_context(c))
        return -1;
#endif
    return 0;
}

int resume(ludp_ctx *c)
{
    if (!c) {
        errno = EINVAL;
        return -1;
    }
    return resume_known_stops(c);
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
    if (!c || !c->attached) {
        errno = EINVAL;
        return -1;
    }
    if (c->unsafe_state) {
        errno = EIO;
        return -1;
    }
    if (c->stopped)
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

        int wi = tid_index(c, w);
        if (wi >= 0)
            c->thread_stopped[wi] = true;
        update_thread_fsbase(c, w);

        int ev = (st >> 16) & 0xffff;
        if (ev == PTRACE_EVENT_CLONE) {
            /* pthread_create(): new thread is already traced + stopped. */
            unsigned long nt = 0;
            if (ptrace(PTRACE_GETEVENTMSG, w, 0, &nt) == 0 && nt > 0) {
                int ni = add_thread(c, (pid_t)nt);
                if (ni >= 0) {
                    c->seized[ni] = true;   /* attached via TRACECLONE */
                    c->thread_stopped[ni] = false; /* own stop not reaped yet */
                }
            }
        }

        /* Event stops are synthetic SIGTRAPs and must be resumed with no
         * signal; a genuine signal-delivery stop (e.g. SIGSEGV) is forwarded
         * so a crash is not silently swallowed. */
        int sig = ev != 0 ? 0 : WSTOPSIG(st);
        if (ptrace(PTRACE_CONT, w, 0, (void *)(uintptr_t)sig) != 0) {
            if (errno == ESRCH) {
                remove_thread(c, w);
                continue;
            }
            return -1;
        }
        wi = tid_index(c, w);
        if (wi >= 0)
            c->thread_stopped[wi] = false;
    }
    return c->nthreads > 0 ? n : -1;
}

/* ============================ remote syscalls ============================ */

#if defined(__aarch64__)
typedef struct {
    struct user_pt_regs gp;
    int syscall_nr;
} trace_regs;

static int arm64_regset(pid_t tid, unsigned note, void *buf, size_t len,
                        bool write)
{
    struct iovec io = { .iov_base = buf, .iov_len = len };
    if (ptrace(write ? PTRACE_SETREGSET : PTRACE_GETREGSET, tid,
               (void *)(uintptr_t)note, &io) != 0)
        return -1;
    if (io.iov_len != len) {
        errno = ENOTSUP;
        return -1;
    }
    return 0;
}

static bool unsupported_regset(void)
{
    /* Absent extension or older kernel: no expanded state exists to save. */
    return errno == EINVAL || errno == EIO || errno == ENODEV;
}

static int arm64_check_context(ludp_ctx *c)
{
    /* The baseline AAPCS64 backend preserves FPSIMD, not scalable SVE/SME
     * state. Inspect activity rather than HWCAP: SVE-capable machines may
     * still run ordinary FPSIMD programs. Newer notes are stable UAPI IDs. */
    const unsigned note_ssve = 0x40b, note_za = 0x40c, note_gcs = 0x410;
    for (int i = 0; i < c->nthreads; ++i) {
        if (!c->seized[i] || !c->thread_stopped[i])
            continue;
        struct user_sve_header header;
        if (!arm64_regset(c->tids[i], NT_ARM_SVE, &header,
                          sizeof header, false)) {
            if ((header.flags & SVE_PT_REGS_MASK) == SVE_PT_REGS_SVE) {
                errno = ENOTSUP;
                return -1;
            }
        } else if (!unsupported_regset()) {
            return -1;
        }
        const unsigned notes[] = {note_ssve, note_za};
        for (size_t j = 0; j < sizeof notes / sizeof notes[0]; ++j) {
            if (!arm64_regset(c->tids[i], notes[j], &header,
                              sizeof header, false)) {
                if (header.size > sizeof header) {
                    errno = ENOTSUP;
                    return -1;
                }
            } else if (!unsupported_regset()) {
                return -1;
            }
        }
        u64 gcs[3];
        if (!arm64_regset(c->tids[i], note_gcs, gcs, sizeof gcs, false)) {
            if (gcs[0] & 1) {
                errno = ENOTSUP;
                return -1;
            }
        } else if (!unsupported_regset()) {
            return -1;
        }
    }
    return 0;
}

static int trace_regs_get(pid_t tid, trace_regs *regs)
{
    if (arm64_regset(tid, NT_PRSTATUS, &regs->gp, sizeof regs->gp, false))
        return -1;
    return arm64_regset(tid, NT_ARM_SYSTEM_CALL, &regs->syscall_nr,
                       sizeof regs->syscall_nr, false);
}

static int trace_regs_set(pid_t tid, const trace_regs *regs)
{
    /* NT_PRSTATUS does not include the kernel's syscall restart number. */
    if (arm64_regset(tid, NT_PRSTATUS, (void *)&regs->gp,
                    sizeof regs->gp, true))
        return -1;
    return arm64_regset(tid, NT_ARM_SYSTEM_CALL, (void *)&regs->syscall_nr,
                       sizeof regs->syscall_nr, true);
}

static bool trace_regs_in_user(const trace_regs *regs)
{
    return regs->syscall_nr == -1;
}

static uintptr_t trace_regs_pc(const trace_regs *regs)
{
    return regs->gp.pc;
}

static uintptr_t find_syscall_insn(ludp_ctx *c)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/%d/maps", c->pid);
    FILE *maps = fopen(path, "r");
    if (!maps)
        return 0;
    uintptr_t found = 0;
    /* Some ARM64 vDSOs have no fallback SVC. In that case use an existing
     * libc syscall instruction, never an injected trap or scratch patch. */
    for (int pass = 0; pass < 2 && !found; ++pass) {
        rewind(maps);
        while (!found && fgets(line, sizeof line, maps)) {
            uintptr_t lo, hi;
            char perm[5], name[512] = {0};
            if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %511s",
                       &lo, &hi, perm, name) != 4 || perm[2] != 'x')
                continue;
            const char *base = strrchr(name, '/');
            base = base ? base + 1 : name;
            bool vdso = strcmp(base, "[vdso]") == 0;
            bool libc = !strncmp(base, "libc.so", 7) ||
                        !strncmp(base, "libc-", 5) ||
                        !strncmp(base, "ld-musl-", 8);
            if (!(pass == 0 ? vdso : libc))
                continue;
            uint32_t words[1024];
            for (uintptr_t addr = lo; addr < hi && !found;) {
                size_t len = hi - addr < sizeof words ? hi - addr : sizeof words;
                if (mem_read(c, addr, words, len))
                    break;
                for (size_t i = 0; i < len / sizeof words[0]; ++i) {
                    if (words[i] == UINT32_C(0xd4000001)) { /* svc #0 */
                        found = addr + i * sizeof words[0];
                        break;
                    }
                }
                addr += len;
            }
        }
    }
    fclose(maps);
    if (!found)
        errno = ENOTSUP;
    return found;
}
#else
typedef struct user_regs_struct trace_regs;

static int trace_regs_get(pid_t tid, trace_regs *regs)
{
    return ptrace(PTRACE_GETREGS, tid, 0, regs) == 0 ? 0 : -1;
}

static int trace_regs_set(pid_t tid, const trace_regs *regs)
{
    return ptrace(PTRACE_SETREGS, tid, 0, regs) == 0 ? 0 : -1;
}

static bool trace_regs_in_user(const trace_regs *regs)
{
    return regs->orig_rax == (unsigned long)-1;
}

static uintptr_t trace_regs_pc(const trace_regs *regs)
{
    return regs->rip;
}

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
#endif

/* PTRACE_O_TRACESYSGOOD makes syscall stops unambiguous.  An asynchronous
 * signal is deferred to resume()/detach() instead of being mistaken for a
 * syscall entry/exit and corrupting the saved register transaction. */
static int wait_syscall_stop(ludp_ctx *c, pid_t tid, int timeout_ms)
{
    stop_wait wait;
    if (stop_wait_init(&wait, timeout_ms)) return -1;
    for (;;) {
        int st = 0;
        pid_t w = waitpid(tid, &st, __WALL | WNOHANG);
        if (w == 0 || (w < 0 && errno == EINTR)) {
            if (stop_wait_pause(&wait)) return -1;
            continue;
        }
        if (w < 0) {
            return -1;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            remove_thread(c, tid);
            errno = ESRCH;
            return -1;
        }
        if (!WIFSTOPPED(st)) {
            errno = EIO;
            return -1;
        }

        int i = tid_index(c, tid);
        if (i >= 0)
            c->thread_stopped[i] = true;
        if (WSTOPSIG(st) == (SIGTRAP | 0x80))
            return st;

        int ev = (st >> 16) & 0xffff;
        if (ev == 0 && i >= 0 && c->pending_sigs[i] == 0)
            c->pending_sigs[i] = WSTOPSIG(st);
        errno = ev == 0 ? EINTR : EBUSY;
        return -1;
    }
}

static int remote_syscall(ludp_ctx *c, long nr, unsigned long a1, unsigned long a2,
                          unsigned long a3, unsigned long a4, unsigned long a5,
                          unsigned long a6, uintptr_t *ret)
{
    if (c->unsafe_state) {
        errno = EIO;
        return -1;
    }
    pid_t tid = -1;
    trace_regs orig, regs;
    int ti = -1;
    bool regs_changed = false;
#if defined(__aarch64__)
    u64 saved_sigmask = 0;
    bool mask_changed = false;
#endif

    /*
     * 不在被内核中断的阻塞系统调用上劫持执行流：这会破坏 futex
     * 等系统调用的 ERESTARTSYS 语义，导致应用在 pthread_join 中报错。
     * 只选择已经 ptrace 停止、且当前在用户代码中的线程执行远程 mmap。
     * 找不到安全线程时拒绝插入，而不是悄悄损坏原进程上下文。
     */
    for (int i = 0; i < c->nthreads; ++i) {
        if (!c->seized[i] || !c->thread_stopped[i] || c->pending_sigs[i])
            continue;
        if (trace_regs_get(c->tids[i], &regs) != 0) continue;
        if (trace_regs_in_user(&regs)) {
            tid = c->tids[i];
            ti = i;
            orig = regs;
            break;
        }
    }
    if (tid < 0) { errno = EBUSY; return -1; }
    regs = orig;
#if defined(__aarch64__)
    /* Keep asynchronous handlers out of the temporary register context.
     * Pending signals remain pending and the exact original mask is restored. */
    u64 blocked = UINT64_MAX;
    if (ptrace(PTRACE_GETSIGMASK, tid, sizeof saved_sigmask, &saved_sigmask) ||
        ptrace(PTRACE_SETSIGMASK, tid, sizeof blocked, &blocked))
        return -1;
    mask_changed = true;
    regs.gp.regs[0] = a1; regs.gp.regs[1] = a2; regs.gp.regs[2] = a3;
    regs.gp.regs[3] = a4; regs.gp.regs[4] = a5; regs.gp.regs[5] = a6;
    regs.gp.regs[8] = (unsigned long)nr;
    regs.gp.pc = c->syscall_insn;
#else
    regs.rax = nr;
    regs.rdi = a1; regs.rsi = a2; regs.rdx = a3;
    regs.r10 = a4; regs.r8 = a5; regs.r9 = a6;
    regs.rip = c->syscall_insn;
#endif
    /* A regset transaction can partially succeed, so roll back even if its
     * second ARM64 note fails after writing NT_PRSTATUS. */
    regs_changed = true;
    if (trace_regs_set(tid, &regs) != 0)
        goto fail;

    if (ptrace(PTRACE_SYSCALL, tid, 0, 0) != 0)
        goto fail;
    c->thread_stopped[ti] = false;
    if (wait_syscall_stop(c, tid, 3000) < 0)   /* syscall entry */
        goto fail;

    if (trace_regs_get(tid, &regs) != 0)
        goto fail;
#if defined(__aarch64__)
    regs.gp.pc = orig.gp.pc;
#else
    regs.rip = orig.rip;            /* redirect the return safely */
#endif
    if (trace_regs_set(tid, &regs) != 0)
        goto fail;

    if (ptrace(PTRACE_SYSCALL, tid, 0, 0) != 0)
        goto fail;
    c->thread_stopped[ti] = false;
    if (wait_syscall_stop(c, tid, 3000) < 0)   /* syscall exit */
        goto fail;

    if (trace_regs_get(tid, &regs) != 0)
        goto fail;
    if (ret)
#if defined(__aarch64__)
        *ret = regs.gp.regs[0];
#else
        *ret = regs.rax;
#endif
    if (trace_regs_set(tid, &orig) != 0)
        goto fail;
#if defined(__aarch64__)
    if (ptrace(PTRACE_SETSIGMASK, tid, sizeof saved_sigmask, &saved_sigmask))
        goto fail;
#endif
    return 0;

fail: {
        int err = errno ? errno : EIO;
        ti = tid_index(c, tid);
        if (regs_changed && ti >= 0 && !c->thread_stopped[ti]) {
            if (ptrace(PTRACE_INTERRUPT, tid, 0, 0) == 0) {
                int st = wait_stop(tid, 3000);
                if (st >= 0 && WIFSTOPPED(st)) {
                    c->thread_stopped[ti] = true;
                    int ev = (st >> 16) & 0xffff;
                    if (ev == 0 && c->pending_sigs[ti] == 0)
                        c->pending_sigs[ti] = WSTOPSIG(st);
                }
            }
        }
        ti = tid_index(c, tid);
        if (regs_changed && ti >= 0 &&
            (!c->thread_stopped[ti] || trace_regs_set(tid, &orig) != 0)) {
            c->unsafe_state = true;
            err = EIO;
        }
#if defined(__aarch64__)
        if (mask_changed && ti >= 0 && c->thread_stopped[ti] &&
            ptrace(PTRACE_SETSIGMASK, tid, sizeof saved_sigmask, &saved_sigmask)) {
            c->unsafe_state = true;
            err = EIO;
        }
#endif
        errno = err;
        return -1;
    }
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
        trace_regs regs;
        if (!c->seized[i]) continue;
        if (trace_regs_get(c->tids[i], &regs)) return -1;
        if (trace_regs_in_user(&regs)) has_user_thread = true;
        uintptr_t pc = trace_regs_pc(&regs);
        for (owned_mapping *m = c->mappings; m; m = m->next) {
            if (m->executable && pc >= m->base && pc - m->base < m->size) {
                errno = EBUSY;
                return -1;
            }
        }
    }
    for (int i = 0; i < c->nprobes; ++i) {
        if (c->probes[i].used && probe_callback_idle(c, &c->probes[i]))
            return -1;
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
    if (rc || (long)ret < 0) {
        if (!rc && (long)ret < 0)
            errno = (int)-(long)ret;
        free(m);
        return -1;
    }
    record_mapping(c, m, ret, size, prot);
    if (out)
        *out = ret;
    return 0;
}

#ifdef MADV_HUGEPAGE
static bool threads_allow_optional_syscalls(const ludp_ctx *c)
{
    if (!c->nthreads) return false;
    for (int i = 0; i < c->nthreads; ++i) {
        if (!c->seized[i] || !c->thread_stopped[i]) return false;
        char path[96], line[256];
        snprintf(path, sizeof path, "/proc/%d/task/%d/status", c->pid, c->tids[i]);
        FILE *status = fopen(path, "r");
        if (!status) return false;
        bool known_unfiltered = false;
        while (fgets(line, sizeof line, status)) {
            if (strncmp(line, "Seccomp:", 8)) continue;
            char *end;
            errno = 0;
            long mode = strtol(line + 8, &end, 10);
            if (end != line + 8 && !errno) {
                while (isspace((unsigned char)*end)) ++end;
                known_unfiltered = mode == 0 && *end == '\0';
            }
            break;
        }
        if (ferror(status)) known_unfiltered = false;
        fclose(status);
        if (!known_unfiltered) return false;
    }
    return true;
}
#endif

/* A million-call capture touches hundreds of megabytes of event storage.
 * Transparent huge pages reduce both first-touch work and final page-table
 * teardown. This is only a hint: disabled/unavailable THP keeps normal pages
 * and identical event capacity. Never hide a ptrace/register transaction
 * failure as an unsupported advisory operation. */
static int advise_ring_pages(ludp_ctx *c, size_t size)
{
#ifdef MADV_HUGEPAGE
    const char *option = getenv("LUDP_HUGEPAGES");
    if (size >= 2u * 1024u * 1024u && (!option || strcmp(option, "0"))) {
        uintptr_t result;
        int saved_errno = errno;
        /* seccomp may kill or trap instead of returning an errno. Never
         * execute an optional syscall unless every stopped thread is known
         * to have no filter. Unknown /proc state also means skip the hint. */
        if (!threads_allow_optional_syscalls(c)) {
            errno = saved_errno;
            return 0;
        }
        if (remote_syscall(c, __NR_madvise, c->ring_addr, size,
                           MADV_HUGEPAGE, 0, 0, 0, &result))
            return -1;
        /* Any kernel rejection of this optional hint is harmless. */
        errno = saved_errno;
    }
#else
    (void)c;
    (void)size;
#endif
    return 0;
}

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/*
 * Allocate an arena inside the architecture's direct-branch range, including
 * space for the trampoline's branch back. Must be called while the tracee's
 * threads are stopped (same context as remote_mmap during attach).
 */
int remote_mmap_near(ludp_ctx *c, uintptr_t target, size_t size, int prot,
                     uintptr_t *out)
{
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || ((size_t)page_size & ((size_t)page_size - 1))) {
        errno = EINVAL;
        return -1;
    }
    size_t page_mask = (size_t)page_size - 1;
    if (size > SIZE_MAX - page_mask) {
        errno = EOVERFLOW;
        return -1;
    }
    size = (size + page_mask) & ~page_mask;
    if (!size)
        size = (size_t)page_size;

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

#if defined(__aarch64__)
    const uintptr_t LIMIT = 0x07000000UL;   /* 112 MiB; B imm26 reaches 128 MiB */
#else
    const uintptr_t LIMIT = 0x78000000UL;   /* 1.875 GiB; JMP rel32 reaches 2 GiB */
#endif
    const uintptr_t STEP = 0x100000UL;      /* 1 MB scan granularity */

    owned_mapping *m = malloc(sizeof *m);
    if (!m) return -1;
    for (int dir = 0; dir < 2; dir++) {
        for (uintptr_t off = 0; off <= LIMIT; off += STEP) {
            int64_t cand = dir == 0 ? (int64_t)target + (int64_t)off
                                    : (int64_t)target - (int64_t)off;
            if (cand <= 0x10000)
                continue;
            uintptr_t p = ((uintptr_t)cand + page_mask) & ~(uintptr_t)page_mask;
            if (p > UINTPTR_MAX - size)
                continue;
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
                if (ret != p) { errno = EIO; return -1; }
                if (out)
                    *out = p;
                return 0;
            }
        }
    }
    free(m);
    errno = ENOMEM;
    return -1;
}

int remote_mprotect(ludp_ctx *c, uintptr_t addr, size_t len, int prot)
{
    uintptr_t ret = 0;
    if (remote_syscall(c, __NR_mprotect, addr, len, prot, 0, 0, 0, &ret) != 0)
        return -1;
    if ((long)ret == 0)
        return 0;
    if ((long)ret < 0)
        errno = (int)-(long)ret;
    else
        errno = EIO;
    return -1;
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
#if defined(__aarch64__)
        l.off_exit = (int)(ludp_tramp_exit - ludp_tramp_start);
#endif
        l.off_ring = (int)(ludp_tramp_ring_ptr - ludp_tramp_start);
        l.off_pid = (int)(ludp_tramp_probe_id - ludp_tramp_start);
        l.off_rdfsbase = (int)(ludp_tramp_rdfsbase - ludp_tramp_start);
        l.off_return_rdfsbase = (int)(ludp_tramp_return_rdfsbase - ludp_tramp_start);
        l.off_slots = (int)(ludp_tramp_slots - ludp_tramp_start);
        l.off_flags = (int)(ludp_tramp_flags - ludp_tramp_start);
        l.off_filter_op = (int)(ludp_tramp_filter_op - ludp_tramp_start);
        l.off_filter_arg = (int)(ludp_tramp_filter_arg - ludp_tramp_start);
        l.off_filter_value = (int)(ludp_tramp_filter_value - ludp_tramp_start);
        l.off_filter_mask = (int)(ludp_tramp_filter_mask - ludp_tramp_start);
        l.off_call_fn = (int)(ludp_tramp_call_fn - ludp_tramp_start);
        l.off_call_data = (int)(ludp_tramp_call_data - ludp_tramp_start);
        l.off_call_active = (int)(ludp_tramp_call_active - ludp_tramp_start);
        l.off_xsave_mode = (int)(ludp_tramp_xsave_mode - ludp_tramp_start);
        l.off_xsave_size = (int)(ludp_tramp_xsave_size - ludp_tramp_start);
        l.off_xsave_mask = (int)(ludp_tramp_xsave_mask - ludp_tramp_start);
        init = 1;
    }
    return &l;
}

int tramp_build(ludp_ctx *c, uintptr_t tramp_addr, u64 probe_id,
                uintptr_t ring_addr, uintptr_t ret_addr,
                const uint8_t *reloc_bytes, int reloc_len, unsigned flags,
                const ludp_filter *filter, const ludp_call_action *call)
{
    const tramp_layout_t *tl = tramp_layout();
    int total = tl->total_size;
    uint8_t *buf = malloc((size_t)total);
    if (!buf)
        return -1;

    memcpy(buf, ludp_tramp_start, (size_t)total);

    memcpy(buf + tl->off_ret, &ret_addr, 8);
#if defined(__aarch64__)
    uint32_t branch;
    if (aarch64_branch(tramp_addr + tl->off_exit, ret_addr, &branch)) {
        free(buf);
        return -1;
    }
    memcpy(buf + tl->off_exit, &branch, sizeof branch);
#endif
    memcpy(buf + tl->off_ring, &ring_addr, 8);
    memcpy(buf + tl->off_pid, &probe_id, 8);

    u64 f = flags;
    memcpy(buf + tl->off_slots, &c->return_slots, 8);
    memcpy(buf + tl->off_flags, &f, 8);
    ludp_filter normalized = {0};
    if (filter)
        normalized = *filter;
    u64 filter_op = normalized.op;
    u64 filter_arg = normalized.arg_index;
    memcpy(buf + tl->off_filter_op, &filter_op, 8);
    memcpy(buf + tl->off_filter_arg, &filter_arg, 8);
    memcpy(buf + tl->off_filter_value, &normalized.value, 8);
    memcpy(buf + tl->off_filter_mask, &normalized.mask, 8);
    ludp_call_action normalized_call = {0};
    if (call)
        normalized_call = *call;
    memcpy(buf + tl->off_call_fn, &normalized_call.function, 8);
    memcpy(buf + tl->off_call_data, &normalized_call.user_data, 8);
    u64 call_active = 0;
    memcpy(buf + tl->off_call_active, &call_active, 8);
    u64 xsave_mode = c->xsave ? 1 : 0;
    memcpy(buf + tl->off_xsave_mode, &xsave_mode, 8);
    memcpy(buf + tl->off_xsave_size, &c->xsave_size, 8);
    memcpy(buf + tl->off_xsave_mask, &c->xsave_mask, 8);

#if defined(__x86_64__)
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

    int rc = mem_write(c, tramp_addr, buf, (size_t)total);
    free(buf);
    return rc;
}

#if defined(__x86_64__)
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

static u64 monotonic_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (u64)ts.tv_sec * UINT64_C(1000000000) + (u64)ts.tv_nsec;
}

static int clock_sample(u64 *tsc, u64 *mono_ns)
{
    u64 before = monotonic_now_ns();
    if (!before)
        return -1;
    u64 cycles = rdtsc_now();
    u64 after = monotonic_now_ns();
    if (!after || after < before)
        return -1;
    *tsc = cycles;
    *mono_ns = before + (after - before) / 2;
    return 0;
}

static int calibrate_monotonic_clock(ludp_ctx *c)
{
#if defined(__aarch64__)
    /* The architectural counter has a specified, invariant frequency. Use
     * it directly, avoiding a sleep-based estimate of the scale factor. */
    u64 frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (!frequency || clock_sample(&c->clock_tsc_anchor,
                                   &c->clock_mono_anchor)) {
        errno = EIO;
        return -1;
    }
    c->clock_ns_per_tsc = 1000000000.0L / (long double)frequency;
#else
    u64 tsc0, ns0, tsc1, ns1;
    if (clock_sample(&tsc0, &ns0))
        return -1;
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 5000000};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    if (clock_sample(&tsc1, &ns1) || tsc1 <= tsc0 || ns1 <= ns0) {
        errno = EIO;
        return -1;
    }
    c->clock_tsc_anchor = tsc1;
    c->clock_mono_anchor = ns1;
    c->clock_ns_per_tsc = (long double)(ns1 - ns0) /
                          (long double)(tsc1 - tsc0);
#endif
    c->clock_ready = true;
    return 0;
}

static u64 tsc_to_monotonic_ns(const ludp_ctx *c, u64 tsc)
{
    if (!c->clock_ready)
        return 0;
    long double delta = tsc >= c->clock_tsc_anchor
                            ? (long double)(tsc - c->clock_tsc_anchor)
                            : -(long double)(c->clock_tsc_anchor - tsc);
    long double value = (long double)c->clock_mono_anchor +
                        delta * c->clock_ns_per_tsc;
    if (value <= 0)
        return 0;
    if (value >= (long double)UINT64_MAX)
        return UINT64_MAX;
    return (u64)(value + 0.5L);
}

static u64 kernel_tid_for_fsbase(const ludp_ctx *c, u64 fsbase)
{
    if (!fsbase)
        return 0;
    for (int i = 0; i < c->nthreads; ++i)
        if (c->fsbases[i] == fsbase)
            return (u64)c->tids[i];
    return 0;
}

/* Drain the consecutive published prefix. Producers drop on full and cannot
 * reuse any of these payloads until our final watermark store. */
int events_read(ludp_ctx *c, ludp_event *out, int max, int *n)
{
    enum { EVENT_CHUNK = 256 };
    u64 flags[EVENT_CHUNK];
    ludp_raw_event raw[EVENT_CHUNK];
    if (!c || max < 0 || (max > 0 && !out)) {
        errno = EINVAL;
        return -1;
    }
    if (n) *n = 0;
    int count = 0;
    u64 consumed = c->consumed;

    while (count < max) {
        u64 idx = consumed & c->ring_mask;
        u64 to_end = c->ring_cap - idx;
        u64 want = (u64)(max - count);
        u64 chunk = want < to_end ? want : to_end;
        if (chunk > EVENT_CHUNK)
            chunk = EVENT_CHUNK;
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

        /* Pair the producer's release publication with the following remote
         * payload loads. ARM64 cannot rely on x86's load ordering here. */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        /* Bulk-read the matching event payloads. */
        uintptr_t ea = c->ring_addr + c->events_off + idx * LUDP_EVENT_SZ;
        if (mem_read(c, ea, raw, (size_t)ready * LUDP_EVENT_SZ) != 0)
            return -1;

        for (u64 k = 0; k < ready; ++k) {
            ludp_event *event = &out[count + k];
            memcpy(event, &raw[k], sizeof raw[k]);
            event->monotonic_ns = tsc_to_monotonic_ns(c, raw[k].tsc);
            event->kernel_tid = kernel_tid_for_fsbase(c, raw[k].tid);
            u64 id = event->probe_id;
            /* IDs normally match allocation order. Avoid a registry scan per
             * event on the high-volume path; failed insertions may leave gaps. */
            int pi = id > 0 && id <= (u64)c->nprobes &&
                     c->probes[id - 1].used && c->probes[id - 1].id == id
                         ? (int)id - 1 : find_probe(c, id);
            if (pi >= 0 && event->type == LUDP_EVENT_ENTRY)
                c->probes[pi].hits++;
        }
        consumed += ready;
        count += (int)ready;
    }

    /* Publish our watermark so producers can compute overflow accurately. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
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

static void configure_xsave(ludp_ctx *c)
{
    /* FXSAVE is the universal x86-64 fallback. XSAVE is used only for the
     * floating/vector components relevant to transparent callback execution;
     * large AMX tile state is intentionally excluded from target-stack use. */
    c->xsave = false;
    c->xsave_size = 512;
    c->xsave_mask = 0;

#if defined(__x86_64__)
    unsigned eax, ebx, ecx, edx;
    if (__get_cpuid_max(0, NULL) < 0x0d ||
        !__get_cpuid(1, &eax, &ebx, &ecx, &edx) ||
        !(ecx & (1u << 26)) || !(ecx & (1u << 27)))
        return;

    uint32_t xcr0_lo, xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    u64 mask = (((u64)xcr0_hi << 32) | xcr0_lo) & UINT64_C(0xe7);
    if ((mask & 3) != 3)
        return;

    size_t need = 576; /* legacy region + standard-format XSAVE header */
    for (unsigned bit = 2; bit <= 7; ++bit) {
        if (!(mask & (UINT64_C(1) << bit)))
            continue;
        __cpuid_count(0x0d, bit, eax, ebx, ecx, edx);
        (void)ecx;
        (void)edx;
        if (!eax) {
            mask &= ~(UINT64_C(1) << bit);
            continue;
        }
        size_t end = (size_t)ebx + (size_t)eax;
        if (end > need)
            need = end;
    }
    need = (need + 63) & ~(size_t)63;
    if (need > 16384)
        return;
    c->xsave = true;
    c->xsave_size = need;
    c->xsave_mask = mask;
#endif
}

int return_slots_init(ludp_ctx *c)
{
    if (c->slots_ready) return 0;
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
    if (!rc) rc = mem_write(c, c->return_slots, buf, size);
    c->slots_ready = rc == 0;
    free(buf);
    return rc;
}

ludp_ctx *ludp_attach(pid_t pid)
{
    int attach_error;
    if (pid <= 0) {
        errno = EINVAL;
        return NULL;
    }
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
    configure_xsave(c);

    if (seize_thread(pid, false) != 0) {
        close(c->memfd);
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
    if (advise_ring_pages(c, ring_bytes))
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
    if (calibrate_monotonic_clock(c) != 0)
        goto fail;
    c->attached = true;
    c->stopped = true;   /* still quiesced; caller resumes when ready */
    c->next_id = 1;
    return c;

fail:
    attach_error = errno ? errno : EIO;
    if (c->mappings && !c->unsafe_state) release_idle_storage(c, true);
    /* A failed register rollback cannot safely be followed by DETACH,
     * which resumes the scratch context. Keep it stopped under EXITKILL. */
    for (int i = 0; !c->unsafe_state && i < c->nthreads; i++) {
        if (!c->seized[i])
            continue;
        if (!c->thread_stopped[i] &&
            ptrace(PTRACE_INTERRUPT, c->tids[i], 0, 0) == 0) {
            int st = wait_stop(c->tids[i], 3000);
            if (st >= 0 && WIFSTOPPED(st)) {
                c->thread_stopped[i] = true;
                int ev = (st >> 16) & 0xffff;
                if (ev == 0 && c->pending_sigs[i] == 0)
                    c->pending_sigs[i] = WSTOPSIG(st);
            }
        }
        /* A later initialization step can fail after quiesce() has observed a
         * real signal-delivery stop.  Detaching must not silently consume it. */
        if (c->thread_stopped[i])
            ptrace(PTRACE_DETACH, c->tids[i], 0,
                   (void *)(uintptr_t)c->pending_sigs[i]);
    }
    if (c->memfd >= 0)
        close(c->memfd);
    free(c->tids);
    free(c->seized);
    free(c->thread_stopped);
    free(c->pending_sigs);
    free(c->fsbases);
    while (c->mappings) {
        owned_mapping *next = c->mappings->next;
        free(c->mappings);
        c->mappings = next;
    }
    free(c);
    errno = attach_error;
    return NULL;
}

int ludp_detach(ludp_ctx *c)
{
    if (!c) return 0;
    if (c->unsafe_state) { errno = EIO; return -1; }
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
        int sig = c->pending_sigs[i];
        if (ptrace(PTRACE_DETACH, c->tids[i], 0,
                   (void *)(uintptr_t)sig) && errno != ESRCH)
            return -1;
        c->seized[i] = false;
        c->thread_stopped[i] = false;
        c->pending_sigs[i] = 0;
    }
    if (c->memfd >= 0)
        close(c->memfd);
    free(c->tids);
    free(c->seized);
    free(c->thread_stopped);
    free(c->pending_sigs);
    free(c->fsbases);
    free(c);
    return 0;
}

/* ============================ public wrappers ============================ */

static int require_ctx(const ludp_ctx *c)
{
    if (!c || !c->attached || c->memfd < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int ludp_resolve(ludp_ctx *c, const char *lib, const char *sym, uintptr_t *addr)
{
    if (require_ctx(c) || !sym || !*sym || !addr) {
        if (c && c->attached && c->memfd >= 0)
            errno = EINVAL;
        return -1;
    }
    return elf_resolve(c->pid, lib, sym, addr, NULL, 0);
}

int ludp_probe_add(ludp_ctx *c, const char *lib, const char *sym, u64 *id_out)
{
    uintptr_t addr;
    if (require_ctx(c) || !sym || !*sym) {
        if (c && c->attached && c->memfd >= 0)
            errno = EINVAL;
        return -1;
    }
    if (elf_resolve(c->pid, lib, sym, &addr, NULL, 0) != 0)
        return -1;
    return probe_add_internal(c, sym, (lib && lib[0]) ? lib : "", addr,
                              LUDP_PROBE_ENTRY, NULL, NULL, id_out);
}

int ludp_probe_add_addr(ludp_ctx *c, const char *sym, uintptr_t addr, u64 *id_out)
{
    if (require_ctx(c) || !sym || !*sym || !addr) {
        if (c && c->attached && c->memfd >= 0)
            errno = EINVAL;
        return -1;
    }
    return probe_add_internal(c, sym, "", addr, LUDP_PROBE_ENTRY, NULL, NULL,
                              id_out);
}

int ludp_probe_add_ex(ludp_ctx *c, const char *lib, const char *sym, unsigned flags, u64 *id_out)
{
    uintptr_t addr;
    if (require_ctx(c) || !sym || !*sym) {
        if (c && c->attached && c->memfd >= 0)
            errno = EINVAL;
        return -1;
    }
    if (elf_resolve(c->pid, lib, sym, &addr, NULL, 0)) return -1;
    return probe_add_internal(c, sym, lib ? lib : "", addr, flags, NULL, NULL,
                              id_out);
}

int ludp_probe_add_addr_ex(ludp_ctx *c, const char *sym, uintptr_t addr, unsigned flags, u64 *id_out)
{
    if (require_ctx(c) || !sym || !*sym || !addr) {
        if (c && c->attached && c->memfd >= 0)
            errno = EINVAL;
        return -1;
    }
    return probe_add_internal(c, sym, "", addr, flags, NULL, NULL, id_out);
}

int ludp_probe_add_filtered_ex(ludp_ctx *c, const char *lib, const char *sym,
                               unsigned flags, const ludp_filter *filter,
                               u64 *id_out)
{
    uintptr_t addr;
    if (require_ctx(c) || !sym || !*sym || !filter) {
        if (c && c->attached && c->memfd >= 0)
            errno = EINVAL;
        return -1;
    }
    if (elf_resolve(c->pid, lib, sym, &addr, NULL, 0))
        return -1;
    return probe_add_internal(c, sym, lib ? lib : "", addr, flags,
                              filter, NULL, id_out);
}

int ludp_probe_add_addr_filtered_ex(ludp_ctx *c, const char *sym,
                                    uintptr_t addr, unsigned flags,
                                    const ludp_filter *filter, u64 *id_out)
{
    if (require_ctx(c) || !sym || !*sym || !addr || !filter) {
        errno = EINVAL;
        return -1;
    }
    return probe_add_internal(c, sym, "", addr, flags, filter, NULL, id_out);
}

int ludp_probe_add_call_ex(ludp_ctx *c, const char *lib, const char *sym,
                           unsigned flags, const ludp_filter *filter,
                           const ludp_call_action *call, u64 *id_out)
{
    uintptr_t addr;
    if (require_ctx(c) || !sym || !*sym || !call || !call->function) {
        errno = EINVAL;
        return -1;
    }
    if (elf_resolve(c->pid, lib, sym, &addr, NULL, 0)) {
        return -1;
    }
    return probe_add_internal(c, sym, lib ? lib : "", addr, flags,
                              filter, call, id_out);
}

int ludp_probe_add_addr_call_ex(ludp_ctx *c, const char *sym,
                                uintptr_t addr, unsigned flags,
                                const ludp_filter *filter,
                                const ludp_call_action *call, u64 *id_out)
{
    if (require_ctx(c) || !sym || !*sym || !addr || !call ||
        !call->function) {
        errno = EINVAL;
        return -1;
    }
    return probe_add_internal(c, sym, "", addr, flags, filter, call, id_out);
}

u64 ludp_return_missed(ludp_ctx *c)
{
    if (require_ctx(c) || !c->ring_addr)
        return 0;
    return mem_read_u64(c, c->ring_addr + 168);
}

int ludp_probe_remove(ludp_ctx *c, u64 id)
{
    if (require_ctx(c) || !id) {
        errno = EINVAL;
        return -1;
    }
    return probe_remove_internal(c, id);
}

int ludp_probe_enable(ludp_ctx *c, u64 id)
{
    if (require_ctx(c) || !id) {
        errno = EINVAL;
        return -1;
    }
    return probe_set_enabled(c, id, true);
}

int ludp_probe_disable(ludp_ctx *c, u64 id)
{
    if (require_ctx(c) || !id) {
        errno = EINVAL;
        return -1;
    }
    return probe_set_enabled(c, id, false);
}

int ludp_probe_count(ludp_ctx *c)
{
    if (require_ctx(c))
        return -1;
    int n = 0;
    for (int i = 0; i < c->nprobes; i++)
        if (c->probes[i].used)
            n++;
    return n;
}

int ludp_probe_list(ludp_ctx *c, ludp_probe_info *out, int max)
{
    if (require_ctx(c) || max < 0 || (max > 0 && !out)) {
        errno = EINVAL;
        return -1;
    }
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
        out[n].flags = p->flags;
        out[n].filter = p->filter;
        out[n].call = p->call;
        out[n].generation = p->generation;
        out[n].hits = p->hits;
        n++;
    }
    return n;
}

int ludp_probe_get_config(ludp_ctx *c, u64 id, ludp_probe_config *out)
{
    if (require_ctx(c) || !id || !out) {
        errno = EINVAL;
        return -1;
    }
    int i = find_probe(c, id);
    if (i < 0) {
        errno = ENOENT;
        return -1;
    }
    probe_t *p = &c->probes[i];
    *out = (ludp_probe_config){
        .flags = p->flags,
        .enabled = p->enabled,
        .filter = p->filter,
        .call = p->call,
    };
    return 0;
}

int ludp_probe_update(ludp_ctx *c, u64 id, const ludp_probe_config *config)
{
    if (require_ctx(c) || !id || !config) {
        errno = EINVAL;
        return -1;
    }
    return probe_update_internal(c, id, config);
}

int ludp_probe_set_filter(ludp_ctx *c, u64 id, const ludp_filter *filter)
{
    ludp_probe_config config;
    if (ludp_probe_get_config(c, id, &config))
        return -1;
    config.filter = filter ? *filter : (ludp_filter){0};
    return probe_update_internal(c, id, &config);
}

int ludp_probe_set_call(ludp_ctx *c, u64 id, const ludp_call_action *call)
{
    ludp_probe_config config;
    if (ludp_probe_get_config(c, id, &config))
        return -1;
    config.call = call ? *call : (ludp_call_action){0};
    return probe_update_internal(c, id, &config);
}

int ludp_events_read(ludp_ctx *c, ludp_event *out, int max, int *n)
{
    if (require_ctx(c) || max < 0 || (max > 0 && !out)) {
        if (n) *n = 0;
        errno = EINVAL;
        return -1;
    }
    return events_read(c, out, max, n);
}

u64 ludp_overflow(ludp_ctx *c)
{
    if (require_ctx(c) || !c->ring_addr)
        return 0;
    return mem_read_u64(c, c->ring_addr + RO_OVERFLOW);
}

u64 ludp_committed(ludp_ctx *c)
{
    if (require_ctx(c) || !c->ring_addr)
        return 0;
    return mem_read_u64(c, c->ring_addr + RO_COMMITTED);
}

int ludp_event_format_perf(const ludp_event *event,
                           const ludp_probe_info *probe, pid_t pid, int cpu,
                           const char *comm, char *buf, size_t cap)
{
    if (!event || !buf || cap == 0 || !event->monotonic_ns ||
        (event->type != LUDP_EVENT_ENTRY &&
         event->type != LUDP_EVENT_RETURN)) {
        errno = EINVAL;
        return -1;
    }
    const char *name = probe && probe->id == event->probe_id
                           ? probe->sym : "probe";
    const char *kind = event->type == LUDP_EVENT_ENTRY ? "entry" : "return";
    u64 seconds = event->monotonic_ns / UINT64_C(1000000000);
    u64 nanos = event->monotonic_ns % UINT64_C(1000000000);
    u64 tid = event->kernel_tid ? event->kernel_tid : (u64)pid;
    int printed;
    if (event->type == LUDP_EVENT_ENTRY) {
        printed = snprintf(buf, cap,
            "%-16.16s %d/%llu [%03d] %llu.%09llu: ludp:%s_%s: "
            "probe_id=%llu call_id=%llu arg0=0x%llx arg1=0x%llx\n",
            comm && *comm ? comm : "ludp", (int)pid,
            (unsigned long long)tid, cpu < 0 ? 0 : cpu,
            (unsigned long long)seconds, (unsigned long long)nanos,
            name, kind, (unsigned long long)event->probe_id,
            (unsigned long long)event->call_id,
            (unsigned long long)event->args[0],
            (unsigned long long)event->args[1]);
    } else {
        printed = snprintf(buf, cap,
            "%-16.16s %d/%llu [%03d] %llu.%09llu: ludp:%s_%s: "
            "probe_id=%llu call_id=%llu retval=0x%llx\n",
            comm && *comm ? comm : "ludp", (int)pid,
            (unsigned long long)tid, cpu < 0 ? 0 : cpu,
            (unsigned long long)seconds, (unsigned long long)nanos,
            name, kind, (unsigned long long)event->probe_id,
            (unsigned long long)event->call_id,
            (unsigned long long)event->retval);
    }
    if (printed < 0 || (size_t)printed >= cap) {
        errno = ENOSPC;
        return -1;
    }
    return printed;
}

int ludp_quiesce(ludp_ctx *c)
{
    if (require_ctx(c))
        return -1;
    return quiesce(c);
}

int ludp_resume(ludp_ctx *c)
{
    if (require_ctx(c))
        return -1;
    return resume(c);
}

int ludp_thread_count(ludp_ctx *c)
{
    if (require_ctx(c))
        return -1;
    return c->nthreads;
}

int ludp_mem_read(ludp_ctx *c, uintptr_t addr, void *buf, size_t len)
{
    if (require_ctx(c))
        return -1;
    return mem_read(c, addr, buf, len);
}

int ludp_mem_write(ludp_ctx *c, uintptr_t addr, const void *buf, size_t len)
{
    if (require_ctx(c))
        return -1;
    return mem_write(c, addr, buf, len);
}

u64 rdtsc_now(void)
{
#if defined(__aarch64__)
    u64 counter;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(counter) :: "memory");
    return counter;
#else
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((u64)hi << 32) | lo;
#endif
}
