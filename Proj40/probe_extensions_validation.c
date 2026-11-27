#define _GNU_SOURCE
#include "ludp.h"
#include "validation_arch.h"
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { MAX_EVENTS = 256 };

typedef struct {
    atomic_int ready, phase, done, release, errors;
} control_t;

__attribute__((noinline, noclone))
static uint64_t advanced_target(uint64_t a, uint64_t b, uint64_t c,
                                uint64_t d, uint64_t e, uint64_t f)
{
    return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13;
}

static uint64_t expected(uint64_t a)
{
    return a + 2 * 3 + 3 * 5 + 4 * 7 + 5 * 11 + 6 * 13;
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void child_phase(control_t *ctl, int phase, int begin, int end)
{
    while (atomic_load(&ctl->phase) != phase) validation_cpu_relax();
    for (int i = begin; i < end; ++i) {
        uint64_t got = advanced_target((uint64_t)i, 2, 3, 4, 5, 6);
        if (got != expected((uint64_t)i))
            atomic_fetch_add(&ctl->errors, 1);
    }
    atomic_store(&ctl->done, phase);
}

static void run_child(control_t *ctl)
{
    /* Publish a user-space safe point before the parent starts ptrace. */
    atomic_store(&ctl->ready, 1);
    child_phase(ctl, 1, 0, 100);
    child_phase(ctl, 2, 0, 100);
    child_phase(ctl, 3, 0, 64);
    child_phase(ctl, 4, 0, 6);
    child_phase(ctl, 5, 0, 6);
    child_phase(ctl, 6, 0, 6);
    child_phase(ctl, 7, 0, 6);
    child_phase(ctl, 8, 0, 6);
    child_phase(ctl, 9, 0, 16);
    child_phase(ctl, 10, 7, 8);
    while (!atomic_load(&ctl->release)) validation_cpu_relax();
    _exit(atomic_load(&ctl->errors) != 0);
}

static int wait_phase(ludp_ctx *ctx, control_t *ctl, int phase)
{
    uint64_t deadline = monotonic_ns() + UINT64_C(10000000000);
    while (atomic_load(&ctl->done) != phase && monotonic_ns() < deadline) {
        if (ludp_poll(ctx) < 0)
            return -1;
        usleep(100);
    }
    return atomic_load(&ctl->done) == phase ? 0 : -1;
}

static int drain(ludp_ctx *ctx, ludp_event out[MAX_EVENTS])
{
    int total = 0;
    for (;;) {
        int n = 0;
        if (ludp_events_read(ctx, out + total, MAX_EVENTS - total, &n))
            return -1;
        total += n;
        if (!n)
            return total;
        if (total == MAX_EVENTS) {
            errno = ENOSPC;
            return -1;
        }
    }
}

static int get_info(ludp_ctx *ctx, u64 id, ludp_probe_info *info)
{
    ludp_probe_info list[4];
    int n = ludp_probe_list(ctx, list, 4);
    for (int i = 0; i < n; ++i) {
        if (list[i].id == id) {
            *info = list[i];
            return 0;
        }
    }
    return -1;
}

int main(void)
{
    int failed = 0, status = 0;
    bool detached = false;
    unsigned char original[8], actual[8];
    memcpy(original, (const void *)(uintptr_t)advanced_target, sizeof original);

    control_t *ctl = mmap(NULL, sizeof(*ctl), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED)
        return 1;
    pid_t child = fork();
    if (child < 0) {
        munmap(ctl, sizeof(*ctl));
        return 1;
    }
    if (child == 0)
        run_child(ctl);

    uint64_t ready_deadline = monotonic_ns() + UINT64_C(10000000000);
    while (!atomic_load(&ctl->ready) && monotonic_ns() < ready_deadline)
        usleep(100);
    if (!atomic_load(&ctl->ready)) {
        fputs("FAIL: child did not reach the initial user-space safe point\n", stderr);
        failed = 1;
        goto finish;
    }

    ludp_ctx *ctx = ludp_attach(child);
    if (!ctx) {
        perror("ludp_attach");
        failed = 1;
        goto finish;
    }

    ludp_filter filter = {
        .op = LUDP_FILTER_EQ,
        .arg_index = 0,
        .value = 42,
    };
    u64 id = 0;
    if (ludp_probe_add_addr_filtered_ex(
            ctx, "advanced_target", (uintptr_t)advanced_target,
            LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN, &filter, &id) ||
        ludp_resume(ctx)) {
        failed = 1;
        goto cleanup_ctx;
    }

    ludp_event events[MAX_EVENTS];
    uint64_t phase_start = monotonic_ns();
    atomic_store(&ctl->phase, 1);
    if (wait_phase(ctx, ctl, 1)) {
        failed = 1;
        goto cleanup_ctx;
    }
    uint64_t phase_end = monotonic_ns();
    int n = drain(ctx, events);
    if (n != 2 || events[0].type != LUDP_EVENT_ENTRY ||
        events[1].type != LUDP_EVENT_RETURN ||
        events[0].probe_id != id || events[1].probe_id != id ||
        events[0].args[0] != 42 || events[1].args[0] != 42 ||
        !events[0].call_id || events[1].call_id != events[0].call_id ||
        events[1].retval != expected(42) ||
        events[0].monotonic_ns < phase_start - UINT64_C(1000000000) ||
        events[1].monotonic_ns > phase_end + UINT64_C(1000000000) ||
        events[0].monotonic_ns > events[1].monotonic_ns ||
        (events[0].kernel_tid && events[0].kernel_tid != (u64)child)) {
        failed = 1;
        goto cleanup_ctx;
    }

    ludp_probe_info info;
    char perf_line[512];
    if (get_info(ctx, id, &info) || info.generation != 1 ||
        info.filter.op != LUDP_FILTER_EQ ||
        ludp_event_format_perf(&events[0], &info, child, 3, "advanced",
                               perf_line, sizeof perf_line) < 0 ||
        !strstr(perf_line, "advanced_target_entry") ||
        !strstr(perf_line, "[003]") || !strstr(perf_line, "probe_id=")) {
        failed = 1;
        goto cleanup_ctx;
    }

    ludp_probe_config unchanged;
    if (ludp_probe_get_config(ctx, id, &unchanged) ||
        ludp_probe_update(ctx, id, &unchanged) ||
        get_info(ctx, id, &info) || info.generation != 1) {
        failed = 1;
        goto cleanup_ctx;
    }

    ludp_probe_config config = {
        .flags = LUDP_PROBE_ENTRY,
        .enabled = true,
        .filter = {.op = LUDP_FILTER_GE, .arg_index = 0, .value = 90},
    };
    if (ludp_probe_update(ctx, id, &config)) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 2);
    if (wait_phase(ctx, ctl, 2) || (n = drain(ctx, events)) != 10) {
        failed = 1;
        goto cleanup_ctx;
    }
    for (int i = 0; i < n; ++i)
        if (events[i].type != LUDP_EVENT_ENTRY || events[i].args[0] != (u64)(90 + i))
            failed = 1;

    filter = (ludp_filter){
        .op = LUDP_FILTER_MASK_EQ,
        .arg_index = 0,
        .value = 3,
        .mask = 15,
    };
    if (failed || ludp_probe_set_filter(ctx, id, &filter)) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 3);
    if (wait_phase(ctx, ctl, 3) || (n = drain(ctx, events)) != 4) {
        failed = 1;
        goto cleanup_ctx;
    }
    const u64 expected_masked[4] = {3, 19, 35, 51};
    for (int i = 0; i < n; ++i)
        if (events[i].args[0] != expected_masked[i])
            failed = 1;

    /* Cover every A2 comparison opcode and the last SysV INTEGER argument.
     * Each update happens while the child is outside the target function. */
    filter = (ludp_filter){
        .op = LUDP_FILTER_EQ,
        .arg_index = 5,
        .value = 6,
    };
    if (failed || ludp_probe_set_filter(ctx, id, &filter)) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 4);
    if (wait_phase(ctx, ctl, 4) || (n = drain(ctx, events)) != 6) {
        failed = 1;
        goto cleanup_ctx;
    }
    for (int i = 0; i < n; ++i)
        if (events[i].args[0] != (u64)i || events[i].args[5] != 6)
            failed = 1;

    filter = (ludp_filter){
        .op = LUDP_FILTER_NE,
        .arg_index = 0,
        .value = 3,
    };
    if (failed || ludp_probe_set_filter(ctx, id, &filter)) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 5);
    if (wait_phase(ctx, ctl, 5) || (n = drain(ctx, events)) != 5) {
        failed = 1;
        goto cleanup_ctx;
    }
    const u64 expected_ne[5] = {0, 1, 2, 4, 5};
    for (int i = 0; i < n; ++i)
        if (events[i].args[0] != expected_ne[i])
            failed = 1;

    filter = (ludp_filter){
        .op = LUDP_FILTER_LT,
        .arg_index = 0,
        .value = 3,
    };
    if (failed || ludp_probe_set_filter(ctx, id, &filter)) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 6);
    if (wait_phase(ctx, ctl, 6) || (n = drain(ctx, events)) != 3) {
        failed = 1;
        goto cleanup_ctx;
    }
    for (int i = 0; i < n; ++i)
        if (events[i].args[0] != (u64)i)
            failed = 1;

    filter.op = LUDP_FILTER_LE;
    if (failed || ludp_probe_set_filter(ctx, id, &filter)) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 7);
    if (wait_phase(ctx, ctl, 7) || (n = drain(ctx, events)) != 4) {
        failed = 1;
        goto cleanup_ctx;
    }
    for (int i = 0; i < n; ++i)
        if (events[i].args[0] != (u64)i)
            failed = 1;

    filter.op = LUDP_FILTER_GT;
    if (failed || ludp_probe_set_filter(ctx, id, &filter)) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 8);
    if (wait_phase(ctx, ctl, 8) || (n = drain(ctx, events)) != 2 ||
        events[0].args[0] != 4 || events[1].args[0] != 5) {
        failed = 1;
        goto cleanup_ctx;
    }

    if (failed || ludp_probe_get_config(ctx, id, &config)) {
        failed = 1;
        goto cleanup_ctx;
    }
    config.enabled = false;
    if (ludp_probe_update(ctx, id, &config)) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 9);
    if (wait_phase(ctx, ctl, 9) || (n = drain(ctx, events)) != 0) {
        failed = 1;
        goto cleanup_ctx;
    }

    ludp_probe_info before_bad, after_bad;
    if (get_info(ctx, id, &before_bad)) {
        failed = 1;
        goto cleanup_ctx;
    }
    config.filter = (ludp_filter){
        .op = LUDP_FILTER_EQ,
        .arg_index = 6,
        .value = 1,
    };
    errno = 0;
    if (ludp_probe_update(ctx, id, &config) == 0 || errno != EINVAL ||
        get_info(ctx, id, &after_bad) ||
        after_bad.generation != before_bad.generation ||
        after_bad.enabled != before_bad.enabled ||
        memcmp(&after_bad.filter, &before_bad.filter,
               sizeof after_bad.filter) != 0) {
        failed = 1;
        goto cleanup_ctx;
    }

    config = (ludp_probe_config){
        .flags = LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN,
        .enabled = true,
        .filter = {0},
    };
    if (ludp_probe_update(ctx, id, &config)) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 10);
    if (wait_phase(ctx, ctl, 10) || (n = drain(ctx, events)) != 2 ||
        events[0].type != LUDP_EVENT_ENTRY ||
        events[1].type != LUDP_EVENT_RETURN ||
        events[0].args[0] != 7 || events[1].retval != expected(7) ||
        ludp_overflow(ctx) || ludp_return_missed(ctx) ||
        atomic_load(&ctl->errors)) {
        failed = 1;
        goto cleanup_ctx;
    }
    if (get_info(ctx, id, &info) || info.id != id || info.generation != 10 ||
        info.flags != (LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN) ||
        !info.enabled || info.filter.op != LUDP_FILTER_NONE)
        failed = 1;

cleanup_ctx:
    if (ctx && ludp_detach(ctx))
        failed = 1;
    else if (ctx)
        detached = true;
    if (detached && !failed) {
        struct iovec local = {actual, sizeof actual};
        struct iovec remote = {
            (void *)(uintptr_t)advanced_target, sizeof actual
        };
        if (process_vm_readv(child, &local, 1, &remote, 1, 0) !=
                (ssize_t)sizeof actual ||
            memcmp(actual, original, sizeof actual))
            failed = 1;
    }

finish:
    if (failed)
        kill(child, SIGKILL);
    else
        atomic_store(&ctl->release, 1);
    waitpid(child, &status, 0);
    if (!failed && (!WIFEXITED(status) || WEXITSTATUS(status)))
        failed = 1;
    munmap(ctl, sizeof(*ctl));
    puts(failed ? "A2/A3/A5 advanced suite: FAIL" :
                  "A2/A3/A5 advanced suite: PASS");
    return failed;
}
