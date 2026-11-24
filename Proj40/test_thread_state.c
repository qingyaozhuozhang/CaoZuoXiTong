#define _GNU_SOURCE
/* 回归测试：对已经运行的多线程进程附加，覆盖阻塞 futex 的主线程。 */
#include "ludp_internal.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

enum { NTHREADS = 4, NCALLS = 1000 };
typedef struct { atomic_int ready, go, completed, joined, release, errors; } state_t;
static state_t *state;
static uint64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void *worker(void *arg) {
    (void)arg;
    atomic_fetch_add(&state->ready, 1);
    while (!atomic_load(&state->go)) __asm__ volatile("pause");
    pid_t expected = getpid();
    for (int i = 1; i < NCALLS; ++i)
        if (getpid() != expected) atomic_fetch_add(&state->errors, 1);
    atomic_fetch_add(&state->completed, 1);
    return NULL;
}
static int run_case(int leader_in_futex) {
    int failed = 0, status = 0;
    state = mmap(NULL, sizeof(*state), PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (state == MAP_FAILED) return 1;
    pid_t pid = fork();
    if (pid < 0) { munmap(state, sizeof(*state)); return 1; }
    if (pid == 0) {
        pthread_t threads[NTHREADS];
        for (int i = 0; i < NTHREADS; ++i)
            if (pthread_create(&threads[i], NULL, worker, NULL)) _exit(2);
        /* 第二种场景：主线程在 pthread_join/futex 中阻塞。 */
        if (!leader_in_futex)
            while (atomic_load(&state->completed) != NTHREADS)
                __asm__ volatile("pause");
        for (int i = 0; i < NTHREADS; ++i) pthread_join(threads[i], NULL);
        atomic_store(&state->joined, 1);
        while (!atomic_load(&state->release)) __asm__ volatile("pause");
        _exit(atomic_load(&state->errors) != 0);
    }
    uint64_t deadline = now_ms() + 10000;
    while (atomic_load(&state->ready) != NTHREADS && now_ms() < deadline)
        usleep(100);
    if (atomic_load(&state->ready) != NTHREADS) { failed = 1; goto cleanup; }
    ludp_ctx *ctx = ludp_attach(pid);
    if (!ctx) { perror("ludp_attach"); failed = 1; goto cleanup; }
    if (ctx->nthreads != NTHREADS + 1 || ctx->tids[0] != pid) failed = 1;
    for (int i = 0; i < ctx->nthreads; ++i) {
        if (!ctx->seized[i]) failed = 1;
        for (int j = 0; j < i; ++j)
            if (ctx->tids[j] == ctx->tids[i]) failed = 1;
    }
    u64 id = 0;
    /* This child was forked without exec and shares the parent's libc layout.
     * The on-disk filename varies between glibc releases. */
    uintptr_t getpid_addr = (uintptr_t)dlsym(RTLD_DEFAULT, "getpid");
    if (!failed && ludp_probe_add_addr_ex(ctx, "getpid", getpid_addr,
                    LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN, &id)) failed = 1;
    if (!failed) {
        atomic_store(&state->go, 1);
        if (ludp_resume(ctx)) failed = 1;
        deadline = now_ms() + 10000;
        while (!atomic_load(&state->joined) && now_ms() < deadline) {
            ludp_poll(ctx);
            usleep(100);
        }
        if (!atomic_load(&state->joined)) failed = 1;
        /* 工作线程完成 join 后，处理尚未到达的 ptrace 退出事件。 */
        if (!failed) {
            uint64_t reap_deadline = now_ms() + 2000;
            while (ctx->nthreads > 1 && now_ms() < reap_deadline) {
                ludp_poll(ctx);
                if (threads_refresh(ctx)) { failed = 1; break; }
                usleep(1000);
            }
        }
        if (!failed && (ctx->nthreads != 1 || ctx->tids[0] != pid || !ctx->seized[0]))
            failed = 1;
        int count = 0, n = 0;
        ludp_event events[256];
        if (!failed) do {
            if (ludp_events_read(ctx, events, 256, &n)) { failed = 1; break; }
            for (int i = 0; i < n; ++i) {
                if (events[i].probe_id != id ||
                    (events[i].type == LUDP_EVENT_RETURN && events[i].retval != (u64)pid))
                    failed = 1;
            }
            count += n;
        } while (n);
        if (!failed && (count != 2 * NTHREADS * NCALLS || ludp_overflow(ctx) ||
                        ludp_return_missed(ctx) || atomic_load(&state->errors))) failed = 1;
    }
    ludp_detach(ctx);
cleanup:
    if (failed) kill(pid, SIGKILL);
    else atomic_store(&state->release, 1);
    waitpid(pid, &status, 0);
    if (!failed && (!WIFEXITED(status) || WEXITSTATUS(status))) failed = 1;
    munmap(state, sizeof(*state));
    printf("multithread %s: %s\n", leader_in_futex ? "futex-blocked leader" : "active leader",
           failed ? "FAIL" : "PASS");
    return failed;
}
int main(void) { return run_case(0) | run_case(1); }
