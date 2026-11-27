#define _GNU_SOURCE
/* 中文演示：两个共享库函数的真实入口事件、六参数和返回值。 */
#include "ludp.h"
#include "validation_arch.h"
#include <errno.h>
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
typedef struct { atomic_int ready, go, done, release, error; } control_t;
static unsigned long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    control_t *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED) return 1;
    pid_t pid = fork();
    if (pid < 0) { munmap(s, sizeof(*s)); return 1; }
    if (pid == 0) {
        atomic_store(&s->ready, 1);
        while (!atomic_load(&s->go)) validation_cpu_relax();
        if (six(1, 2, 3, 4, 5, 6) != 183 || floating(2.5) != 4.0)
            atomic_store(&s->error, 1);
        atomic_store(&s->done, 1);
        while (!atomic_load(&s->release)) validation_cpu_relax();
        _exit(atomic_load(&s->error) != 0);
    }

    int failed = 0, status = 0, stops = 0;
    unsigned entries[2] = {0}, returns[2] = {0};
    u64 ids[2] = {0};
    const char *names[2] = {"six", "floating"};
    const u64 expected_args[6] = {1, 2, 3, 4, 5, 6};
    uintptr_t addresses[2] = {0};
    unsigned char original[2][16], patched[VALIDATION_ENTRY_READ_SIZE];
    ludp_event entry[2] = {{0}};
    unsigned long long ready_deadline = now_ms() + 10000;
    while (!atomic_load(&s->ready) && now_ms() < ready_deadline) usleep(100);
    if (!atomic_load(&s->ready)) { failed = 1; goto finish; }
    ludp_ctx *ctx = ludp_attach(pid);
    if (!ctx) { perror("ludp_attach"); failed = 1; goto finish; }
    for (int i = 0; i < 2; ++i) {
        if (ludp_resolve(ctx, "libprobe_targets.so", names[i], &addresses[i]) ||
            ludp_mem_read(ctx, addresses[i], original[i], sizeof original[i]) ||
            ludp_probe_add_ex(ctx, "libprobe_targets.so", names[i],
                              LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN, &ids[i]) ||
            ludp_mem_read(ctx, addresses[i], patched, sizeof patched)) {
            perror("探针安装/入口读取失败"); failed = 1; goto detach;
        }
        if (!validation_entry_is_branch(patched)) {
            fprintf(stderr, "FAIL: %s 入口不是 %s\n", names[i], VALIDATION_BRANCH_NAME);
            failed = 1; goto detach;
        }
        printf("F1：pid=%ld libprobe_targets.so!%s 地址=0x%llx probe_id=%llu\n",
               (long)pid, names[i], (unsigned long long)addresses[i],
               (unsigned long long)ids[i]);
        fputs("    原始入口=", stdout);
        for (int j = 0; j < VALIDATION_ENTRY_READ_SIZE; ++j)
            printf("%s%02x", j ? " " : "", original[i][j]);
        fputs("；探针入口=", stdout);
        for (int j = 0; j < VALIDATION_ENTRY_READ_SIZE; ++j)
            printf("%s%02x", j ? " " : "", patched[j]);
        printf("（%s）\n", VALIDATION_BRANCH_NAME);
    }
    atomic_store(&s->go, 1);
    if (ludp_resume(ctx)) { failed = 1; goto detach; }
    unsigned long long deadline = now_ms() + 10000;
    while (!atomic_load(&s->done) && now_ms() < deadline) {
        int n = ludp_poll(ctx);
        if (n < 0) { failed = 1; goto detach; }
        stops += n;
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
            int k = ev->probe_id == ids[0] ? 0 : ev->probe_id == ids[1] ? 1 : -1;
            if (k < 0 || !ev->call_id || !ev->ret_addr) {
                fputs("FAIL: 未知探针或事件配对信息无效\n", stderr);
                failed = 1; continue;
            }
            if (k == 0 && memcmp(ev->args, expected_args, sizeof expected_args)) {
                fputs("FAIL: six 参数与目标进程实际调用不符\n", stderr);
                failed = 1;
            }
            if (ev->type == LUDP_EVENT_ENTRY) {
                entries[k]++;
                entry[k] = *ev;
                if (k == 0) {
                    printf("F2：six 六个整数参数 = [%llu, %llu, %llu, %llu, %llu, %llu]，call_id=%llu\n",
                           (unsigned long long)ev->args[0], (unsigned long long)ev->args[1],
                           (unsigned long long)ev->args[2], (unsigned long long)ev->args[3],
                           (unsigned long long)ev->args[4], (unsigned long long)ev->args[5],
                           (unsigned long long)ev->call_id);
                } else printf("入口：floating，call_id=%llu\n", (unsigned long long)ev->call_id);
            } else if (ev->type == LUDP_EVENT_RETURN) {
                returns[k]++;
                if (entries[k] != 1 || entry[k].call_id != ev->call_id ||
                    entry[k].tid != ev->tid || entry[k].ret_addr != ev->ret_addr ||
                    memcmp(entry[k].args, ev->args, sizeof ev->args)) {
                    fputs("FAIL: 入口/返回事件配对失败\n", stderr);
                    failed = 1;
                }
                if (k == 0) {
                    printf("F3：six 返回值（" VALIDATION_INTEGER_REGISTER "）= %llu，call_id=%llu\n",
                           (unsigned long long)ev->retval, (unsigned long long)ev->call_id);
                    if (ev->retval != 183) failed = 1;
                }
                if (k == 1) {
                    double x = 0;
                    memcpy(&x, ev->fp_ret, sizeof(x));
                    printf("F3：floating 返回值（" VALIDATION_VECTOR_REGISTER "）= %.2f，call_id=%llu\n",
                           x, (unsigned long long)ev->call_id);
                    if (x != 4.0) failed = 1;
                }
            } else { fputs("FAIL: 未知事件类型\n", stderr); failed = 1; }
        }
    }
    printf("事件核验：six=%u 入口/%u 返回，floating=%u 入口/%u 返回，丢失=%llu，返回丢失=%llu，运行期 ptrace 停止=%d\n",
           entries[0], returns[0], entries[1], returns[1],
           (unsigned long long)ludp_overflow(ctx),
           (unsigned long long)ludp_return_missed(ctx), stops);
    if (entries[0] != 1 || returns[0] != 1 || entries[1] != 1 || returns[1] != 1 ||
        ludp_overflow(ctx) || ludp_return_missed(ctx) || stops || atomic_load(&s->error)) failed = 1;
    for (int i = 0; i < 2; ++i) {
        unsigned char restored[16];
        if (ludp_probe_remove(ctx, ids[i]) ||
            ludp_mem_read(ctx, addresses[i], restored, sizeof restored) ||
            memcmp(original[i], restored, sizeof restored)) {
            fputs("FAIL: 入口字节恢复失败\n", stderr);
            failed = 1;
        } else printf("清理：%s 原始入口 16 字节校验通过。\n", names[i]);
    }
detach:
    if (ludp_detach(ctx)) { perror("ludp_detach"); failed = 1; }
    else puts("清理：会话已分离。");
finish:
    if (failed) kill(pid, SIGKILL);
    else atomic_store(&s->release, 1);
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) failed = 1;
    if (!failed && (!WIFEXITED(status) || WEXITSTATUS(status))) failed = 1;
    munmap(s, sizeof(*s));
    printf("F1–F3 演示：%s\n", failed ? "FAIL" : "PASS");
    return failed;
}
