#define _GNU_SOURCE
/* 中文演示：两个共享库函数的真实入口事件、六参数和返回值。 */
#include "ludp_internal.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern uint64_t six(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
extern double floating(double);
typedef struct { atomic_int go, done, release, error; } control_t;
static unsigned long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int main(void) {
    control_t *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED) return 1;
    pid_t pid = fork();
    if (pid < 0) { munmap(s, sizeof(*s)); return 1; }
    if (pid == 0) {
        while (!atomic_load(&s->go)) ludp_cpu_relax();
        if (six(1, 2, 3, 4, 5, 6) != 183 || floating(2.5) != 4.0)
            atomic_store(&s->error, 1);
        atomic_store(&s->done, 1);
        while (!atomic_load(&s->release)) ludp_cpu_relax();
        _exit(atomic_load(&s->error) != 0);
    }

    int failed = 0, status = 0, entries = 0, returns = 0;
    u64 integer_id = 0, fp_id = 0;
    ludp_ctx *ctx = ludp_attach(pid);
    if (!ctx) { perror("ludp_attach"); failed = 1; goto finish; }
    if (ludp_probe_add_ex(ctx, "libtest.so", "six", 3, &integer_id) != 0 ||
        ludp_probe_add_ex(ctx, "libtest.so", "floating", 3, &fp_id) != 0) {
        perror("探针安装失败"); failed = 1; goto detach;
    }
    printf("F1：已向目标进程 %ld 的共享库函数安装用户态跳转探针。\n", (long)pid);
    atomic_store(&s->go, 1);
    if (ludp_resume(ctx)) { failed = 1; goto detach; }
    unsigned long long deadline = now_ms() + 10000;
    while (!atomic_load(&s->done) && now_ms() < deadline) {
        ludp_poll(ctx);
        usleep(100);
    }
    if (!atomic_load(&s->done)) { failed = 1; goto detach; }
    for (;;) {
        ludp_event events[16];
        int n = 0;
        if (ludp_events_read(ctx, events, 16, &n)) { failed = 1; break; }
        if (!n) break;
        for (int i = 0; i < n; ++i) {
            ludp_event *ev = &events[i];
            if (ev->type == LUDP_EVENT_ENTRY) {
                entries++;
                if (ev->probe_id == integer_id) {
                    printf("F2：six 六个整数参数 = [%llu, %llu, %llu, %llu, %llu, %llu]\n",
                           (unsigned long long)ev->args[0], (unsigned long long)ev->args[1],
                           (unsigned long long)ev->args[2], (unsigned long long)ev->args[3],
                           (unsigned long long)ev->args[4], (unsigned long long)ev->args[5]);
                }
            } else if (ev->type == LUDP_EVENT_RETURN) {
                returns++;
                if (ev->probe_id == integer_id)
                    printf("F3：six 整数返回值= %llu，call_id=%llu\n",
                           (unsigned long long)ev->retval, (unsigned long long)ev->call_id);
                if (ev->probe_id == fp_id) {
                    double x = 0;
                    memcpy(&x, ev->xmm0, sizeof(x));
                    printf("F3：floating 浮点返回值= %.2f，call_id=%llu\n",
                           x, (unsigned long long)ev->call_id);
                    if (x != 4.0) failed = 1;
                }
            }
        }
    }
    if (entries != 2 || returns != 2 || ludp_overflow(ctx) ||
        ludp_return_missed(ctx) || atomic_load(&s->error)) failed = 1;
detach:
    ludp_detach(ctx);
finish:
    if (failed) kill(pid, SIGKILL);
    else atomic_store(&s->release, 1);
    waitpid(pid, &status, 0);
    if (!failed && (!WIFEXITED(status) || WEXITSTATUS(status))) failed = 1;
    munmap(s, sizeof(*s));
    printf("F1–F3 演示：%s\n", failed ? "FAIL" : "PASS");
    return failed;
}
