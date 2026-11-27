#define _GNU_SOURCE
#include "ludp_internal.h"
#include "validation_arch.h"
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    atomic_int step, done, entered, hold, quit;
    long result;
} control;
static control *ctl;

__attribute__((noinline, noclone))
static long measured(long x)
{
    atomic_store(&ctl->entered, 1);
    while (atomic_load(&ctl->hold)) validation_cpu_relax();
    return x * 3 + 2;
}

__attribute__((noinline, noclone, used))
static long second_target(long x) { return x + 1; }

static int wait_value(atomic_int *field, int value)
{
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (atomic_load(field) != value) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 5) return -1;
        usleep(100);
    }
    return 0;
}

static void run_child(void)
{
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    atomic_store(&ctl->done, 0);
    int previous = 0;
    while (!atomic_load(&ctl->quit)) {
        int step = atomic_load(&ctl->step);
        if (step != previous) {
            ctl->result = measured(step);
            previous = step;
            atomic_store(&ctl->done, step);
        }
        validation_cpu_relax();
    }
    _exit(0);
}

static int events(ludp_ctx *ctx, u64 id, int step, int count)
{
    ludp_event ev[3];
    int n = -1;
    if (wait_value(&ctl->done, step) || ctl->result != step * 3 + 2 ||
        ludp_events_read(ctx, ev, 3, &n) || n != count) return -1;
    if (!count) return 0;
    return !(ev[0].probe_id == id && ev[0].type == LUDP_EVENT_ENTRY &&
             ev[0].args[0] == (u64)step && ev[1].probe_id == id &&
             ev[1].type == LUDP_EVENT_RETURN && ev[1].call_id == ev[0].call_id &&
             ev[1].retval == (u64)ctl->result);
}

typedef struct { uintptr_t start, end; } interval;
static int unmapped(pid_t pid, const interval *owned, int count)
{
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = 1;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
        for (int i = 0; i < count; ++i)
            if (lo < owned[i].end && hi > owned[i].start) ok = 0;
    }
    if (ferror(f)) ok = 0;
    fclose(f);
    return ok;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "F4/F5 line %d: %s (errno=%d)\n", __LINE__, #x, errno); goto fail; } } while (0)

static int test_session(pid_t child, int *step)
{
    uintptr_t addr = (uintptr_t)measured;
    unsigned char original[16], actual[16];
    memcpy(original, (const void *)addr, sizeof original);
    ludp_ctx *ctx = ludp_attach(child);
    CHECK(ctx);
    u64 id;
    unsigned mode = LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN;
    CHECK(!ludp_probe_add_addr_ex(ctx, "measured", addr, mode, &id));
    interval owned[8];
    int count = 0;
    for (owned_mapping *m = ctx->mappings; m; m = m->next) {
        CHECK(count < 8);
        owned[count++] = (interval){m->base, m->base + m->size};
    }
    CHECK(count >= 3);
    CHECK(!ludp_resume(ctx));
    atomic_store(&ctl->step, ++*step);
    CHECK(!events(ctx, id, *step, 2));
    CHECK(!ludp_probe_disable(ctx, id) && !ludp_probe_disable(ctx, id));
    CHECK(!ludp_mem_read(ctx, addr, actual, sizeof actual));
    CHECK(!memcmp(actual, original, sizeof actual));
    atomic_store(&ctl->step, ++*step);
    CHECK(!events(ctx, id, *step, 0));
    CHECK(!ludp_probe_enable(ctx, id) && !ludp_probe_enable(ctx, id));

    atomic_store(&ctl->entered, 0);
    atomic_store(&ctl->hold, 1);
    atomic_store(&ctl->step, ++*step);
    CHECK(!wait_value(&ctl->entered, 1));
    CHECK(ludp_detach(ctx) == -1 && errno == EBUSY);
    atomic_store(&ctl->hold, 0);
    CHECK(!events(ctx, id, *step, 2));

    CHECK(!ludp_probe_remove(ctx, id));
    CHECK(ludp_probe_count(ctx) == 0);
    CHECK(ludp_probe_enable(ctx, id) == -1 && errno == ENOENT);
    CHECK(!ludp_mem_read(ctx, addr, actual, sizeof actual));
    CHECK(!memcmp(actual, original, sizeof actual));
    /* Detach a second probe while it is still enabled. */
    CHECK(!ludp_probe_add_addr_ex(ctx, "measured", addr, mode, &id));
    for (owned_mapping *m = ctx->mappings; m; m = m->next) {
        CHECK(count < 8);
        owned[count++] = (interval){m->base, m->base + m->size};
    }
    /* Keep one probe installed while another is repeatedly added and removed. */
    CHECK(!ludp_quiesce(ctx));
    uintptr_t first_slot = 0;
    for (int i = 0; i < LUDP_MAX_PROBES + 1; ++i) {
        u64 extra;
        CHECK(!ludp_probe_add_addr(ctx, "second_target", (uintptr_t)second_target, &extra));
        ludp_probe_info info[2];
        CHECK(ludp_probe_list(ctx, info, 2) == 2 && info[1].id == extra);
        if (!i) first_slot = info[1].trampoline;
        CHECK(info[1].trampoline == first_slot);
        CHECK(!ludp_probe_remove(ctx, extra));
    }
    CHECK(!ludp_detach(ctx));
    struct iovec local = {actual, sizeof actual}, remote = {(void *)addr, sizeof actual};
    CHECK(process_vm_readv(child, &local, 1, &remote, 1, 0) == (ssize_t)sizeof actual);
    CHECK(!memcmp(actual, original, sizeof actual));
    CHECK(unmapped(child, owned, count));
    atomic_store(&ctl->step, ++*step);
    CHECK(!wait_value(&ctl->done, *step) && ctl->result == *step * 3 + 2);
    return 0;
fail:
    return -1;
}

static int seccomp_session(void)
{
    atomic_store(&ctl->step, 0);
    atomic_store(&ctl->done, -1);
    atomic_store(&ctl->entered, 0);
    atomic_store(&ctl->hold, 0);
    atomic_store(&ctl->quit, 0);
    ctl->result = 0;
    pid_t child = fork();
    if (!child) {
        /* If attach tries the optional hint, this child is killed instead of
         * returning an errno. All required probe syscalls remain permitted. */
        struct sock_filter instructions[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_madvise, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog program = {
            .len = (unsigned short)(sizeof instructions / sizeof instructions[0]),
            .filter = instructions,
        };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
            prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program))
            _exit(2);
        run_child();
    }
    if (child < 0) return -1;
    int failed = wait_value(&ctl->done, 0), step = 0, status = 0;
    if (!failed && (setenv("LUDP_RING_CAP", "65536", 1) ||
                    setenv("LUDP_HUGEPAGES", "1", 1) ||
                    test_session(child, &step)))
        failed = 1;
    if (failed) kill(child, SIGKILL);
    else atomic_store(&ctl->quit, 1);
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (failed || waited != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
        fputs("FAIL seccomp-protected optional huge-page hint\n", stderr);
        return -1;
    }
    puts("PASS seccomp: madvise would kill target; attach, probes and complete mapping cleanup succeed");
    return 0;
}

int main(void)
{
    ctl = mmap(NULL, sizeof *ctl, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED) return 1;
    atomic_store(&ctl->done, -1);
    pid_t child = fork();
    if (!child) run_child();
    if (child < 0) return 1;
    int failed = wait_value(&ctl->done, 0), step = 0, status = 0;
    /* An unrelated running child must time out without consuming a status.
     * wait_stop uses a wall-clock deadline even when polling is backed off. */
    if (!failed && (wait_stop(child, 2) != -1 || errno != ETIMEDOUT)) {
        fputs("FAIL ptrace-stop timeout contract\n", stderr);
        failed = 1;
    }
    /* Cover small ordinary mappings and both optional large-ring page paths.
     * No assertion depends on the kernel granting the huge-page hint. */
    for (int i = 0; !failed && i < 3; ++i) {
        if (setenv("LUDP_RING_CAP", i ? "65536" : "1024", 1) ||
            setenv("LUDP_HUGEPAGES", i == 2 ? "0" : "1", 1)) {
            failed = 1;
            break;
        }
        failed = test_session(child, &step);
    }
    if (failed) kill(child, SIGKILL);
    else atomic_store(&ctl->quit, 1);
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (failed || waited != child || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    if (!failed && seccomp_session()) failed = 1;
    munmap(ctl, sizeof *ctl);
    if (failed) return 1;
    puts("PASS F4/F5: four sessions, original bytes, mappings, switches, active return, stop timeout and huge-page hint/fallback");
    return 0;
}
