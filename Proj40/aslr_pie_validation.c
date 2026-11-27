#define _GNU_SOURCE
/* Resolve and probe independently exec'd PIE / shared-library mappings. */
#include "ludp.h"
#include "validation_arch.h"
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern uint64_t six(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
typedef struct {
    atomic_int ready, go, done, release, error;
    uintptr_t main_address, library_address;
} control_t;

__attribute__((noinline, noclone, visibility("default")))
uint64_t aslr_target(uint64_t a, uint64_t b, uint64_t c,
                     uint64_t d, uint64_t e, uint64_t f)
{
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6;
}

static uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static int target_main(int fd)
{
    control_t *ctl = mmap(NULL, sizeof(*ctl), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ctl == MAP_FAILED) return 1;
    ctl->main_address = (uintptr_t)aslr_target;
    ctl->library_address = (uintptr_t)six;
    atomic_store(&ctl->ready, 1);
    while (!atomic_load(&ctl->go)) validation_cpu_relax();
    if (aslr_target(1, 2, 3, 4, 5, 6) != 91 || six(1, 2, 3, 4, 5, 6) != 183)
        atomic_store(&ctl->error, 1);
    atomic_store(&ctl->done, 1);
    while (!atomic_load(&ctl->release)) validation_cpu_relax();
    int result = atomic_load(&ctl->error);
    munmap(ctl, sizeof(*ctl));
    return result;
}

static int run_round(unsigned round, uintptr_t addresses[2])
{
    int failed = 1, status = 0, fd = -1;
    pid_t child = -1;
    ludp_ctx *ctx = NULL;
    control_t *ctl = MAP_FAILED;
    const char *libraries[2] = {NULL, "libprobe_targets.so"};
    const char *symbols[2] = {"aslr_target", "six"};
    const u64 arguments[6] = {1, 2, 3, 4, 5, 6};
    u64 ids[2] = {0}, calls[2] = {0};
    unsigned entries[2] = {0}, returns[2] = {0};
    unsigned char original[2][16];
#define REQUIRE(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL: exec round %u line %d: %s (errno=%d)\n", \
            round, __LINE__, #expr, errno); goto cleanup; } } while (0)
    fd = memfd_create("ludp-exec-validation", 0); /* Deliberately inherited across exec. */
    REQUIRE(fd >= 0 && ftruncate(fd, sizeof(*ctl)) == 0);
    ctl = mmap(NULL, sizeof(*ctl), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    REQUIRE(ctl != MAP_FAILED);
    child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        /* Bound a standalone invocation too; the exec'd target inherits both. */
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() == 1) _exit(126);
        alarm(20);
        char descriptor[32];
        snprintf(descriptor, sizeof descriptor, "%d", fd);
        execl("/proc/self/exe", "aslr_pie_validation", "--target", descriptor, (char *)NULL);
        _exit(127);
    }
    uint64_t deadline = now_ms() + 10000;
    while (!atomic_load(&ctl->ready) && now_ms() < deadline) usleep(100);
    REQUIRE(atomic_load(&ctl->ready));
    ctx = ludp_attach(child);
    REQUIRE(ctx != NULL);
    for (int k = 0; k < 2; ++k) {
        REQUIRE(ludp_resolve(ctx, libraries[k], symbols[k], &addresses[k]) == 0);
        REQUIRE(addresses[k] == (k ? ctl->library_address : ctl->main_address));
        REQUIRE(ludp_mem_read(ctx, addresses[k], original[k], sizeof original[k]) == 0);
        REQUIRE(ludp_probe_add_ex(ctx, libraries[k], symbols[k],
                                 LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN, &ids[k]) == 0);
    }
    atomic_store(&ctl->go, 1);
    REQUIRE(ludp_resume(ctx) == 0);
    deadline = now_ms() + 10000;
    while (!atomic_load(&ctl->done) && now_ms() < deadline) {
        REQUIRE(ludp_poll(ctx) == 0);
        usleep(100);
    }
    REQUIRE(atomic_load(&ctl->done) && !atomic_load(&ctl->error));
    for (;;) {
        ludp_event events[8];
        int count = 0;
        REQUIRE(ludp_events_read(ctx, events, 8, &count) == 0);
        if (!count) break;
        for (int i = 0; i < count; ++i) {
            ludp_event *event = &events[i];
            int k = event->probe_id == ids[0] ? 0 : event->probe_id == ids[1] ? 1 : -1;
            REQUIRE(k >= 0 && event->call_id && !memcmp(event->args, arguments, sizeof arguments));
            if (event->type == LUDP_EVENT_ENTRY) {
                REQUIRE(++entries[k] == 1);
                calls[k] = event->call_id;
            } else {
                REQUIRE(event->type == LUDP_EVENT_RETURN && ++returns[k] == 1);
                REQUIRE(calls[k] == event->call_id && event->retval == (k ? 183u : 91u));
            }
        }
    }
    REQUIRE(entries[0] == 1 && entries[1] == 1 && returns[0] == 1 && returns[1] == 1);
    REQUIRE(!ludp_overflow(ctx) && !ludp_return_missed(ctx));
    for (int k = 0; k < 2; ++k) {
        unsigned char restored[16];
        REQUIRE(ludp_probe_remove(ctx, ids[k]) == 0);
        REQUIRE(ludp_mem_read(ctx, addresses[k], restored, sizeof restored) == 0);
        REQUIRE(!memcmp(original[k], restored, sizeof restored));
    }
    REQUIRE(ludp_detach(ctx) == 0);
    ctx = NULL;
    atomic_store(&ctl->release, 1);
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    REQUIRE(waited == child);
    child = -1;
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("exec round %u: PIE=0x%llx shared=0x%llx, arguments/returns/restoration PASS\n",
           round, (unsigned long long)addresses[0], (unsigned long long)addresses[1]);
    failed = 0;
cleanup:
    if (ctx) (void)ludp_detach(ctx);
    if (child > 0) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    if (ctl != MAP_FAILED) munmap(ctl, sizeof(*ctl));
    if (fd >= 0) close(fd);
    return failed;
#undef REQUIRE
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--target")) return target_main(atoi(argv[2]));
    if (argc != 1) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    uintptr_t initial[2] = {0}, addresses[2] = {0};
    int varied[2] = {0};
    for (unsigned round = 0; round < 8; ++round) {
        if (run_round(round + 1, addresses)) return 1;
        if (!round) memcpy(initial, addresses, sizeof initial);
        for (int k = 0; k < 2; ++k) varied[k] |= initial[k] != addresses[k];
    }
    if (!varied[0] || !varied[1]) {
        fputs("FAIL: independent exec did not vary both PIE and shared-library bases; check ASLR policy\n", stderr);
        return 1;
    }
    puts("independent exec / ASLR / PIE: PASS (8 fresh images, both mappings varied)");
    return 0;
}
