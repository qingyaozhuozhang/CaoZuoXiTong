#define _GNU_SOURCE
#include "ludp.h"
#include "validation_arch.h"
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

typedef struct {
    atomic_int ready;
    atomic_int release;
} api_control;

__attribute__((noinline, noclone, visibility("default")))
uint64_t api_target(uint64_t value)
{
    return value + 1;
}

#define EXPECT(expr) do {                                                     \
    if (!(expr)) {                                                            \
        fprintf(stderr, "API contract line %d: %s (errno=%d)\n",            \
                __LINE__, #expr, errno);                                      \
        failed = 1;                                                           \
        goto cleanup;                                                         \
    }                                                                         \
} while (0)

int main(void)
{
    int failed = 0, status = 0;
    pid_t child = -1;
    ludp_ctx *ctx = NULL;
    api_control *control = mmap(NULL, sizeof *control,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (control == MAP_FAILED)
        return 1;

    errno = 0;
    EXPECT(ludp_attach(0) == NULL && errno == EINVAL);
    errno = 0;
    EXPECT(ludp_poll(NULL) == -1 && errno == EINVAL);
    errno = 0;
    EXPECT(ludp_probe_count(NULL) == -1 && errno == EINVAL);
    errno = 0;
    EXPECT(ludp_overflow(NULL) == 0 && errno == EINVAL);
    EXPECT(ludp_detach(NULL) == 0);

    child = fork();
    if (child < 0) {
        failed = 1;
        goto cleanup;
    }
    if (child == 0) {
        atomic_store(&control->ready, 1);
        while (!atomic_load(&control->release))
            validation_cpu_relax();
        _exit(api_target(1) != 2);
    }

    struct timespec start, current;
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!atomic_load(&control->ready)) {
        clock_gettime(CLOCK_MONOTONIC, &current);
        EXPECT(current.tv_sec - start.tv_sec < 10);
        usleep(100);
    }
    ctx = ludp_attach(child);
    EXPECT(ctx != NULL);

    uintptr_t resolved = 0;
    EXPECT(ludp_resolve(ctx, NULL, "api_target", &resolved) == 0);
    EXPECT(resolved == (uintptr_t)api_target);

    errno = 0;
    EXPECT(ludp_resolve(ctx, NULL, NULL, &resolved) == -1 && errno == EINVAL);
    errno = 0;
    EXPECT(ludp_resolve(ctx, NULL, "api_target", NULL) == -1 &&
           errno == EINVAL);
    errno = 0;
    EXPECT(ludp_probe_add_addr(ctx, NULL, (uintptr_t)api_target, NULL) == -1 &&
           errno == EINVAL);
    errno = 0;
    EXPECT(ludp_probe_add_addr(ctx, "bad", 0, NULL) == -1 && errno == EINVAL);
    errno = 0;
    EXPECT(ludp_probe_list(ctx, NULL, 1) == -1 && errno == EINVAL);
    errno = 0;
    EXPECT(ludp_probe_list(ctx, NULL, -1) == -1 && errno == EINVAL);
    EXPECT(ludp_probe_list(ctx, NULL, 0) == 0);

    int n = -1;
    EXPECT(ludp_events_read(ctx, NULL, 0, &n) == 0 && n == 0);
    errno = 0;
    EXPECT(ludp_events_read(ctx, NULL, 1, &n) == -1 && errno == EINVAL &&
           n == 0);

    u64 id = 0;
    EXPECT(ludp_probe_add_addr(ctx, "api_target", (uintptr_t)api_target,
                               &id) == 0 && id != 0);
    errno = 0;
    EXPECT(ludp_probe_get_config(ctx, 0, NULL) == -1 && errno == EINVAL);
    errno = 0;
    EXPECT(ludp_probe_remove(ctx, 0) == -1 && errno == EINVAL);
    EXPECT(ludp_probe_remove(ctx, id) == 0);
    EXPECT(ludp_detach(ctx) == 0);
    ctx = NULL;

cleanup:
    if (ctx)
        ludp_detach(ctx);
    if (child > 0) {
        if (failed)
            kill(child, SIGKILL);
        else
            atomic_store(&control->release, 1);
        waitpid(child, &status, 0);
        if (!failed && (!WIFEXITED(status) || WEXITSTATUS(status)))
            failed = 1;
    }
    munmap(control, sizeof *control);
    puts(failed ? "public API/ELF contract: FAIL" :
                  "public API/ELF contract: PASS");
    return failed;
}
