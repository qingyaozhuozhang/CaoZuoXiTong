#define _GNU_SOURCE
#include "ludp.h"
#include "validation_arch.h"
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

enum { NTARGETS = 16, ROUNDS = 32, CHAIN_TARGET = 80 };

typedef uint64_t (*target_fn)(uint64_t, uint64_t, uint64_t,
                              uint64_t, uint64_t, uint64_t);

#define DEFINE_TARGET(n)                                                        \
    __attribute__((noinline, noclone)) static uint64_t target_##n(              \
        uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) \
    {                                                                            \
        return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13 + (n);              \
    }

DEFINE_TARGET(0)  DEFINE_TARGET(1)  DEFINE_TARGET(2)  DEFINE_TARGET(3)
DEFINE_TARGET(4)  DEFINE_TARGET(5)  DEFINE_TARGET(6)  DEFINE_TARGET(7)
DEFINE_TARGET(8)  DEFINE_TARGET(9)  DEFINE_TARGET(10) DEFINE_TARGET(11)
DEFINE_TARGET(12) DEFINE_TARGET(13) DEFINE_TARGET(14) DEFINE_TARGET(15)

static target_fn const targets[NTARGETS] = {
    target_0, target_1, target_2, target_3,
    target_4, target_5, target_6, target_7,
    target_8, target_9, target_10, target_11,
    target_12, target_13, target_14, target_15,
};

typedef struct {
    atomic_int ready, phase, done, release, errors;
} control_t;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t expected(uint64_t a, int target)
{
    return a + 2 * 3 + 3 * 5 + 4 * 7 + 5 * 11 + 6 * 13 + (uint64_t)target;
}

static void run_child(control_t *ctl)
{
    atomic_store(&ctl->ready, 1);
    while (atomic_load(&ctl->phase) != 1) validation_cpu_relax();
    for (int round = 0; round < ROUNDS; ++round) {
        for (int i = 0; i < NTARGETS; ++i) {
            uint64_t got = targets[i]((uint64_t)round, 2, 3, 4, 5, 6);
            if (got != expected((uint64_t)round, i))
                atomic_fetch_add(&ctl->errors, 1);
        }
    }
    atomic_store(&ctl->done, 1);
    while (atomic_load(&ctl->phase) != 2) validation_cpu_relax();
    for (int i = 0; i < NTARGETS; ++i) {
        uint64_t got = targets[i](1000, 2, 3, 4, 5, 6);
        if (got != expected(1000, i))
            atomic_fetch_add(&ctl->errors, 1);
    }
    atomic_store(&ctl->done, 2);
    while (atomic_load(&ctl->phase) != 3) validation_cpu_relax();
    uint64_t got = target_0(2000, 2, 3, 4, 5, 6);
    if (got != expected(2000, 0))
        atomic_fetch_add(&ctl->errors, 1);
    atomic_store(&ctl->done, 3);
    while (!atomic_load(&ctl->release)) validation_cpu_relax();
    _exit(atomic_load(&ctl->errors) != 0);
}

static int wait_done(ludp_ctx *ctx, atomic_int *done, int value)
{
    uint64_t deadline = now_ms() + 10000;
    while (atomic_load(done) != value && now_ms() < deadline) {
        if (ludp_poll(ctx) < 0) return -1;
        usleep(100);
    }
    return atomic_load(done) == value ? 0 : -1;
}

static int probe_index(const u64 *ids, int nids, u64 id)
{
    for (int i = 0; i < nids; ++i)
        if (ids[i] == id) return i;
    return -1;
}

static int verify_first_phase(ludp_ctx *ctx, const u64 ids[NTARGETS + 1])
{
    uint64_t seen[NTARGETS + 1] = {0};
    int total = 0;
    for (;;) {
        ludp_event events[128];
        int n = 0;
        if (ludp_events_read(ctx, events, 128, &n)) return -1;
        if (!n) break;
        total += n;
        for (int i = 0; i < n; ++i) {
            ludp_event *event = &events[i];
            int pi = probe_index(ids, NTARGETS + 1, event->probe_id);
            if (pi < 0 || event->type != LUDP_EVENT_ENTRY ||
                !event->ret_addr || event->args[0] >= ROUNDS ||
                event->args[1] != 2 || event->args[2] != 3 ||
                event->args[3] != 4 || event->args[4] != 5 ||
                event->args[5] != 6)
                return -1;
            uint64_t bit = UINT64_C(1) << event->args[0];
            if (seen[pi] & bit) return -1;
            seen[pi] |= bit;
        }
    }
    uint64_t all = (UINT64_C(1) << ROUNDS) - 1;
    if (total != (NTARGETS + 1) * ROUNDS) return -1;
    for (int i = 0; i < NTARGETS + 1; ++i)
        if (seen[i] != all) return -1;
    return ludp_overflow(ctx) ? -1 : 0;
}

static int verify_second_phase(ludp_ctx *ctx, const u64 ids[NTARGETS + 1])
{
    int seen[NTARGETS + 1] = {0};
    int total = 0;
    for (;;) {
        ludp_event events[64];
        int n = 0;
        if (ludp_events_read(ctx, events, 64, &n)) return -1;
        if (!n) break;
        total += n;
        for (int i = 0; i < n; ++i) {
            int pi = probe_index(ids, NTARGETS + 1, events[i].probe_id);
            if (pi < 0 || pi == 5 || pi == NTARGETS || seen[pi] ||
                events[i].type != LUDP_EVENT_ENTRY || events[i].args[0] != 1000)
                return -1;
            seen[pi] = 1;
        }
    }
    if (total != NTARGETS - 1) return -1;
    for (int i = 0; i < NTARGETS; ++i)
        if (seen[i] != (i != 5)) return -1;
    return 0;
}

static int verify_long_chain(ludp_ctx *ctx, const u64 ids[CHAIN_TARGET])
{
    int seen[CHAIN_TARGET] = {0};
    ludp_event events[CHAIN_TARGET + 8];
    int n = 0;
    if (ludp_events_read(ctx, events, CHAIN_TARGET + 8, &n) ||
        n != CHAIN_TARGET)
        return -1;
    for (int i = 0; i < n; ++i) {
        int pi = probe_index(ids, CHAIN_TARGET, events[i].probe_id);
        if (pi < 0 || seen[pi] || events[i].type != LUDP_EVENT_ENTRY ||
            events[i].args[0] != 2000)
            return -1;
        seen[pi] = 1;
    }
    for (int i = 0; i < CHAIN_TARGET; ++i)
        if (!seen[i])
            return -1;
    return 0;
}

int main(void)
{
    int failed = 0, status = 0;
    unsigned char original[NTARGETS][8], actual[8];
    for (int i = 0; i < NTARGETS; ++i)
        memcpy(original[i], (const void *)(uintptr_t)targets[i], sizeof original[i]);

    control_t *ctl = mmap(NULL, sizeof(*ctl), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED) return 1;
    pid_t child = fork();
    if (child < 0) { munmap(ctl, sizeof(*ctl)); return 1; }
    if (child == 0) run_child(ctl);

    uint64_t ready_deadline = now_ms() + 10000;
    while (!atomic_load(&ctl->ready) && now_ms() < ready_deadline) usleep(100);
    if (!atomic_load(&ctl->ready)) { failed = 1; goto finish; }
    ludp_ctx *ctx = ludp_attach(child);
    if (!ctx) { failed = 1; goto finish; }
    u64 ids[NTARGETS + 1] = {0};
    for (int i = 0; i < NTARGETS; ++i) {
        char name[32];
        snprintf(name, sizeof name, "target_%d", i);
        if (ludp_probe_add_addr(ctx, name, (uintptr_t)targets[i], &ids[i])) {
            failed = 1;
            goto detach;
        }
    }
    /* The seventeenth probe shares target_0 and exercises entry-chain rebuild. */
    if (ludp_probe_add_addr(ctx, "target_0_duplicate", (uintptr_t)targets[0],
                            &ids[NTARGETS]) ||
        ludp_probe_count(ctx) != NTARGETS + 1 || ludp_resume(ctx)) {
        failed = 1;
        goto detach;
    }

    atomic_store(&ctl->phase, 1);
    if (wait_done(ctx, &ctl->done, 1) || verify_first_phase(ctx, ids) ||
        atomic_load(&ctl->errors)) {
        failed = 1;
        goto detach;
    }
    if (ludp_quiesce(ctx) || ludp_probe_remove(ctx, ids[NTARGETS]) ||
        ludp_probe_disable(ctx, ids[5]) || ludp_probe_count(ctx) != NTARGETS ||
        ludp_resume(ctx)) {
        failed = 1;
        goto detach;
    }
    atomic_store(&ctl->phase, 2);
    if (wait_done(ctx, &ctl->done, 2) || verify_second_phase(ctx, ids) ||
        atomic_load(&ctl->errors))
        failed = 1;
    if (failed)
        goto detach;

    /* Exercise a same-address chain beyond the old internal 64-entry limit.
     * All 80 enabled probes must remain reachable and emit exactly once. */
    if (ludp_quiesce(ctx)) {
        failed = 1;
        goto detach;
    }
    u64 chain_ids[CHAIN_TARGET] = {0};
    chain_ids[0] = ids[0];
    for (int i = 1; i < CHAIN_TARGET; ++i) {
        char name[32];
        snprintf(name, sizeof name, "target_0_chain_%d", i);
        if (ludp_probe_add_addr(ctx, name, (uintptr_t)targets[0],
                                &chain_ids[i])) {
            failed = 1;
            goto detach;
        }
    }
    if (ludp_resume(ctx)) {
        failed = 1;
        goto detach;
    }
    atomic_store(&ctl->phase, 3);
    if (wait_done(ctx, &ctl->done, 3) || verify_long_chain(ctx, chain_ids) ||
        atomic_load(&ctl->errors))
        failed = 1;

detach:
    if (ludp_detach(ctx)) failed = 1;
    if (!failed) {
        for (int i = 0; i < NTARGETS; ++i) {
            struct iovec local = {actual, sizeof actual};
            struct iovec remote = {(void *)(uintptr_t)targets[i], sizeof actual};
            if (process_vm_readv(child, &local, 1, &remote, 1, 0) !=
                    (ssize_t)sizeof actual ||
                memcmp(actual, original[i], sizeof actual)) {
                failed = 1;
                break;
            }
        }
    }

finish:
    if (failed) kill(child, SIGKILL);
    else atomic_store(&ctl->release, 1);
    waitpid(child, &status, 0);
    if (!failed && (!WIFEXITED(status) || WEXITSTATUS(status))) failed = 1;
    munmap(ctl, sizeof(*ctl));
    puts(failed ? "F6 multi-probe: FAIL" :
                  "F6 multi-probe: PASS (16 addresses + same-address chain)");
    return failed;
}
