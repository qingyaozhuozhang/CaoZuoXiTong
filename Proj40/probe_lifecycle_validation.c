#define _GNU_SOURCE
#include "ludp_internal.h"
#include "validation_arch.h"
#include <errno.h>
#include <dlfcn.h>
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

#if defined(__GLIBC__)
/* musl deliberately keeps dlclose'd DSOs mapped. Exercise actual unload
 * and reuse here on the supported glibc runners. */
typedef struct {
    atomic_int stage, advance;
    uintptr_t entry;
    unsigned char original[16];
} unload_control;

static void run_unload_child(unload_control *sync)
{
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() == 1) _exit(2);
    alarm(20);
    void *handle = dlopen("./libprobe_targets.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) _exit(3);
    sync->entry = (uintptr_t)dlsym(handle, "six");
    if (!sync->entry) _exit(4);
    memcpy(sync->original, (const void *)sync->entry, sizeof sync->original);
    atomic_store(&sync->stage, 1);
    while (atomic_load(&sync->advance) < 1) validation_cpu_relax();
    if (dlclose(handle)) _exit(5);
    atomic_store(&sync->stage, 2);
    while (atomic_load(&sync->advance) < 2) validation_cpu_relax();

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) _exit(6);
    uintptr_t page = sync->entry & ~((uintptr_t)page_size - 1);
    void *replacement = mmap((void *)page, (size_t)page_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                             -1, 0);
    if (replacement != (void *)page) _exit(7);
    memset((void *)sync->entry, 0xa5, sizeof sync->original);
    atomic_store(&sync->stage, 3);
    while (atomic_load(&sync->advance) < 3) validation_cpu_relax();
    if (munmap(replacement, (size_t)page_size)) _exit(8);

    handle = dlopen("./libprobe_targets.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle) _exit(9);
    uint64_t (*six)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) =
        dlsym(handle, "six");
    if (!six) _exit(10);
    sync->entry = (uintptr_t)six;
    atomic_store(&sync->stage, 4);
    while (atomic_load(&sync->advance) < 4) validation_cpu_relax();
    if (six(1, 2, 3, 4, 5, 6) != 183) _exit(11);
    atomic_store(&sync->stage, 5);
    while (atomic_load(&sync->advance) < 5) validation_cpu_relax();
    _exit(dlclose(handle) != 0);
}

static int unload_session(void)
{
    const char *library = "libprobe_targets.so";
    int failed = 1, status = 0;
    pid_t child = -1;
    ludp_ctx *ctx = NULL;
    u64 old[2], replacement_probe;
    uintptr_t resolved;
    unload_control *sync = mmap(NULL, sizeof *sync, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sync == MAP_FAILED) return -1;
#define UNLOAD_CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL: dlclose line %d: %s (errno=%d)\n", \
            __LINE__, #expr, errno); goto finish; } } while (0)
    child = fork();
    UNLOAD_CHECK(child >= 0);
    if (!child) run_unload_child(sync);
    UNLOAD_CHECK(!wait_value(&sync->stage, 1));
    ctx = ludp_attach(child);
    UNLOAD_CHECK(ctx);
    UNLOAD_CHECK(!ludp_probe_add(ctx, library, "six", &old[0]));
    UNLOAD_CHECK(!ludp_probe_add(ctx, library, "six", &old[1]));
    UNLOAD_CHECK(!ludp_resume(ctx));
    atomic_store(&sync->advance, 1);

    UNLOAD_CHECK(!wait_value(&sync->stage, 2));
    UNLOAD_CHECK(ludp_resolve(ctx, library, "six", &resolved) == -1);
    UNLOAD_CHECK(ludp_probe_enable(ctx, old[0]) == -1 && errno == ESTALE);
    UNLOAD_CHECK(!ludp_probe_remove(ctx, old[0]));
    atomic_store(&sync->advance, 2);
    UNLOAD_CHECK(!wait_value(&sync->stage, 3));

    /* Deliberately reuse the entry address for tracee-owned data. */
    unsigned char actual[sizeof sync->original], marker[sizeof sync->original];
    memset(marker, 0xa5, sizeof marker);
    UNLOAD_CHECK(ludp_probe_set_filter(ctx, old[1], NULL) == -1 && errno == ESTALE);
    UNLOAD_CHECK(!ludp_probe_remove(ctx, old[1]) && ludp_probe_count(ctx) == 0);
    UNLOAD_CHECK(!ludp_mem_read(ctx, sync->entry, actual, sizeof actual) &&
                 !memcmp(actual, marker, sizeof actual));
    atomic_store(&sync->advance, 3);
    UNLOAD_CHECK(!wait_value(&sync->stage, 4));

    UNLOAD_CHECK(!ludp_probe_add_ex(ctx, library, "six",
                                   LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN,
                                   &replacement_probe));
    atomic_store(&sync->advance, 4);
    UNLOAD_CHECK(!wait_value(&sync->stage, 5));
    ludp_event events[4];
    int count = 0;
    UNLOAD_CHECK(!ludp_events_read(ctx, events, 4, &count) && count == 2 &&
                 events[0].probe_id == replacement_probe &&
                 events[1].probe_id == replacement_probe && events[1].retval == 183);
    UNLOAD_CHECK(!ludp_probe_remove(ctx, replacement_probe));
    UNLOAD_CHECK(!ludp_mem_read(ctx, sync->entry, actual, sizeof actual) &&
                 !memcmp(actual, sync->original, sizeof actual));
    UNLOAD_CHECK(!ludp_detach(ctx));
    ctx = NULL;
    atomic_store(&sync->advance, 5);
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    UNLOAD_CHECK(waited == child);
    child = -1;
    UNLOAD_CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
    puts("dlclose with active probes, address reuse and reload: PASS");
    failed = 0;
finish:
    if (ctx) (void)ludp_detach(ctx);
    if (child > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    munmap(sync, sizeof *sync);
    return failed;
#undef UNLOAD_CHECK
}
#else
static int unload_session(void)
{
    puts("actual dlclose unload: SKIP (requires glibc)");
    return 0;
}
#endif

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
    if (!failed && unload_session()) failed = 1;
    munmap(ctl, sizeof *ctl);
    if (failed) return 1;
    puts("PASS F4/F5: four sessions, original bytes, mappings, switches, active return, stop timeout and huge-page hint/fallback");
    return 0;
}
