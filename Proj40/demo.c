/*
 * demo.c - F6 + F7 test harness.
 *
 *   demo stress [threads] [seconds]
 *       attach 24 probes across libc/libm (+ one chained second probe on
 *       getpid), run a multi-threaded target, drain events live, then report.
 *
 *   demo bench [iters]
 *       measure getpid loop with no probe vs. with one probe and report the
 *       per-call overhead.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include "ludp.h"

/* ------------------------------------------------------------------ */
/* buffered non-blocking line reader over a pipe                       */
/* ------------------------------------------------------------------ */
typedef struct {
    int fd;
    char buf[4096];
    int pos, len;
} lr_t;

static void lr_init(lr_t *r, int fd)
{
    r->fd = fd;
    r->pos = r->len = 0;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* returns line length; 0 = no complete line yet; -1 = EOF/error */
static int lr_line(lr_t *r, char *out, int cap)
{
    int o = 0;
    for (;;) {
        while (r->pos < r->len) {
            char c = r->buf[r->pos++];
            if (c == '\n') { out[o] = 0; return o; }
            if (o < cap - 1) out[o++] = c;
        }
        r->len = read(r->fd, r->buf, sizeof r->buf);
        r->pos = 0;
        if (r->len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            return -1;
        }
        if (r->len == 0)
            return -1;
    }
}

/* wait up to timeout_ms for one complete line; 0 on timeout, -1 on EOF */
static int lr_get(lr_t *r, char *out, int cap, int timeout_ms)
{
    struct pollfd p = { .fd = r->fd, .events = POLLIN };
    for (;;) {
        int n = lr_line(r, out, cap);
        if (n > 0)
            return n;
        if (n < 0)
            return -1;
        int pr = poll(&p, 1, timeout_ms);
        if (pr <= 0)
            return 0;
    }
}

/* ------------------------------------------------------------------ */
/* target spawn                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    pid_t pid;
    int out_fd;   /* read child stdout */
    int in_fd;    /* write child stdin  */
} child_t;

static int spawn_target(child_t *c, const char *target_path,
                        const char *mode, const char *a1, const char *a2)
{
    int c2p[2], p2c[2];
    if (pipe(c2p) != 0 || pipe(p2c) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        dup2(c2p[1], STDOUT_FILENO);
        dup2(p2c[0], STDIN_FILENO);
        close(c2p[0]); close(c2p[1]);
        close(p2c[0]); close(p2c[1]);
        execl(target_path, target_path, mode, a1, a2, (char *)NULL);
        _exit(127);
    }
    close(c2p[1]);
    close(p2c[0]);
    c->pid = pid;
    c->out_fd = c2p[0];
    c->in_fd = p2c[1];
    return 0;
}

/* ------------------------------------------------------------------ */
/* probe function table                                               */
/* ------------------------------------------------------------------ */
typedef struct { const char *sym; const char *lib; } fnent_t;

static const fnent_t FUNCS[] = {
    { "getpid",  "libc.so.6" }, { "getppid", "libc.so.6" },
    { "getuid",  "libc.so.6" }, { "geteuid", "libc.so.6" },
    { "getgid",  "libc.so.6" }, { "getegid", "libc.so.6" },
    { "gettid",  "libc.so.6" },
    { "malloc",  "libc.so.6" }, { "free",    "libc.so.6" },
    { "calloc",  "libc.so.6" }, { "realloc", "libc.so.6" },
    { "rand",    "libc.so.6" }, { "srand",   "libc.so.6" },
    { "qsort",   "libc.so.6" }, { "getenv",  "libc.so.6" },
    { "abs",     "libc.so.6" }, { "labs",    "libc.so.6" },
    { "atoi",    "libc.so.6" },
    { "sqrt",    "libm.so.6" }, { "log",     "libm.so.6" },
    { "exp",     "libm.so.6" }, { "log2",    "libm.so.6" },
    { "exp2",    "libm.so.6" }, { "fabs",    "libm.so.6" },
    { "cbrt",    "libm.so.6" }, { "hypot",   "libm.so.6" },
    { "pow",     "libm.so.6" }, { "atan2",   "libm.so.6" },
    { "fmod",    "libm.so.6" },
};
#define NFUNCS ((int)(sizeof(FUNCS) / sizeof(FUNCS[0])))

/* ------------------------------------------------------------------ */
/* stress mode                                                        */
/* ------------------------------------------------------------------ */

typedef struct { char sym[128]; uintptr_t addr; u64 id; u64 hits; } pinfo_t;

static int run_stress(const char *target_path, int nthreads, int seconds)
{
    child_t c;
    char tbuf[16], sbuf[16];
    snprintf(tbuf, sizeof tbuf, "%d", nthreads);
    snprintf(sbuf, sizeof sbuf, "%d", seconds);
    if (spawn_target(&c, target_path, "stress", tbuf, sbuf) != 0)
        return -1;

    lr_t lr;
    lr_init(&lr, c.out_fd);
    char line[512];

    /* READY */
    pid_t pid = 0;
    for (int i = 0; i < 5000; i++) {
        int n = lr_get(&lr, line, sizeof line, 20);
        if (n < 0) { fprintf(stderr, "no READY\n"); return -1; } /* EOF only */
        if (n == 0) continue;                                    /* timeout: keep waiting */
        if (strncmp(line, "READY", 5) == 0) { pid = (pid_t)atoi(line + 6); break; }
    }
    if (!pid) { fprintf(stderr, "bad READY\n"); return -1; }

    /* ADDR lines */
    uintptr_t taddr[NFUNCS];
    memset(taddr, 0, sizeof taddr);
    for (int i = 0; i < NFUNCS; i++) {
        char sym[128];
        unsigned long a = 0;
        for (int tries = 0; tries < 200; tries++) {
            if (lr_get(&lr, line, sizeof line, 20) <= 0) continue;
            if (sscanf(line, "ADDR %127s 0x%lx", sym, &a) == 2) {
                for (int k = 0; k < NFUNCS; k++)
                    if (strcmp(FUNCS[k].sym, sym) == 0)
                        taddr[k] = (uintptr_t)a;
                break;
            }
        }
    }

    /* GO flag address */
    uintptr_t go_addr = 0;
    for (int tries = 0; tries < 200; tries++) {
        unsigned long a = 0;
        if (lr_get(&lr, line, sizeof line, 20) <= 0) continue;
        if (sscanf(line, "GO 0x%lx", &a) == 1) { go_addr = (uintptr_t)a; break; }
    }

    ludp_ctx *ctx = ludp_attach(pid);
    if (!ctx) { fprintf(stderr, "attach failed\n"); return -1; }
    fprintf(stderr, "[demo] attached, threads=%d\n", ludp_thread_count(ctx));

    /* resolve + cross-check + add probes */
    pinfo_t pins[NFUNCS + 1];
    int npins = 0;
    printf("\n=== 符号解析交叉验证 (target dlsym vs tracer ELF) ===\n");
    printf("%-10s %-12s %-18s %-18s %s\n", "symbol", "lib", "dlsym", "elf", "match");
    for (int i = 0; i < NFUNCS; i++) {
        uintptr_t raddr = 0;
        int rc = ludp_resolve(ctx, FUNCS[i].lib, FUNCS[i].sym, &raddr);
        if (rc != 0) {
            printf("%-10s %-12s %-18s %-18s %s\n", FUNCS[i].sym, FUNCS[i].lib,
                   "-", rc == -2 ? "IFUNC" : "FAIL", "skip");
            continue;
        }
        u64 id = 0;
        if (ludp_probe_add_addr(ctx, FUNCS[i].sym, raddr, &id) != 0) {
            printf("%-10s %-12s probe add FAIL\n", FUNCS[i].sym, FUNCS[i].lib);
            continue;
        }
        pins[npins].sym[0] = 0;
        snprintf(pins[npins].sym, sizeof pins[npins].sym, "%s", FUNCS[i].sym);
        pins[npins].addr = raddr;
        pins[npins].id = id;
        pins[npins].hits = 0;
        npins++;

        bool match = (taddr[i] != 0 && taddr[i] == raddr);
        printf("%-10s %-12s 0x%-16lx 0x%-16lx %s\n",
               FUNCS[i].sym, FUNCS[i].lib,
               (unsigned long)taddr[i], (unsigned long)raddr,
               match ? "OK" : "MISMATCH");
    }

    /* chain a second probe on getpid to show same-address coexistence */
    uintptr_t gp = 0;
    ludp_resolve(ctx, "libc.so.6", "getpid", &gp);
    u64 id2 = 0;
    if (gp && ludp_probe_add_addr(ctx, "getpid#2", gp, &id2) == 0) {
        snprintf(pins[npins].sym, sizeof pins[npins].sym, "getpid#2");
        pins[npins].addr = gp;
        pins[npins].id = id2;
        pins[npins].hits = 0;
        npins++;
    }
    printf("已添加探针: %d (目标: >=16)\n", ludp_probe_count(ctx));
    printf("线程数: %d\n", ludp_thread_count(ctx));
    fprintf(stderr, "[demo] probes=%d, sending GO (write 0x%lx)\n",
            ludp_probe_count(ctx), (unsigned long)go_addr);

    if (!go_addr) { fprintf(stderr, "no GO address\n"); ludp_detach(ctx); return -1; }
    {
        int one = 1;
        ludp_mem_write(ctx, go_addr, &one, sizeof one);
        ludp_resume(ctx);
    }

    /* drain loop */
    ludp_event *ev = malloc(65536 * sizeof(ludp_event));
    u64 total_events = 0;
    /* O(1) per-event accounting: probe ids are small integers, tids go through
     * a tiny open-addressing hash so the drain never becomes the bottleneck. */
    u64 hits_by_id[512];
    memset(hits_by_id, 0, sizeof hits_by_id);
    enum { TH_SLOTS = 1024 };
    u64 th_tid[TH_SLOTS];
    u64 th_cnt[TH_SLOTS];
    memset(th_tid, 0, sizeof th_tid);
    memset(th_cnt, 0, sizeof th_cnt);
    unsigned long th_iters[64] = {0};
    int iters_received = 0;
    bool done = false;

    time_t t0 = time(NULL);
    int budget = seconds + 20;
    while (!done && (time(NULL) - t0) < budget) {
        /* keep dynamically created worker threads running (F7) */
        ludp_poll(ctx);

        int n = 0;
        ludp_events_read(ctx, ev, 65536, &n);
        for (int i = 0; i < n; i++) {
            total_events++;
            if (ev[i].probe_id < 512)
                hits_by_id[ev[i].probe_id]++;
            u64 h = (ev[i].tid * 0x9E3779B97F4A7C15ull) >> 54;
            int s = (int)h & (TH_SLOTS - 1);
            while (th_tid[s] != 0 && th_tid[s] != ev[i].tid)
                s = (s + 1) & (TH_SLOTS - 1);
            th_tid[s] = ev[i].tid;
            th_cnt[s]++;
        }

        while (lr_line(&lr, line, sizeof line) > 0) {
            int id; unsigned long it;
            if (sscanf(line, "ITER %d %lu", &id, &it) == 2) {
                th_iters[id] = it;
                iters_received++;
            } else if (strncmp(line, "DONE", 4) == 0) {
                done = true;
            }
        }
        usleep(1000);
    }

    fprintf(stderr, "[demo] run phase done=%d events=%llu\n",
            done, (unsigned long long)total_events);

    /* final drain */
    for (;;) {
        int n = 0;
        ludp_events_read(ctx, ev, 65536, &n);
        if (n == 0) break;
        for (int i = 0; i < n; i++) {
            total_events++;
            if (ev[i].probe_id < 512)
                hits_by_id[ev[i].probe_id]++;
            u64 h = (ev[i].tid * 0x9E3779B97F4A7C15ull) >> 54;
            int s = (int)h & (TH_SLOTS - 1);
            while (th_tid[s] != 0 && th_tid[s] != ev[i].tid)
                s = (s + 1) & (TH_SLOTS - 1);
            th_tid[s] = ev[i].tid;
            th_cnt[s]++;
        }
    }

    /* map id-indexed counters back to the probe table for reporting */
    for (int i = 0; i < npins; i++)
        pins[i].hits = (pins[i].id < 512) ? hits_by_id[pins[i].id] : 0;

    /* collect the tid histogram into the reporting table */
    u64 tid_counts[128][2];
    int n_tids = 0;
    for (int s = 0; s < TH_SLOTS && n_tids < 128; s++) {
        if (th_tid[s] == 0)
            continue;
        tid_counts[n_tids][0] = th_tid[s];
        tid_counts[n_tids][1] = th_cnt[s];
        n_tids++;
    }

    printf("\n=== F6: 多探针命中统计 ===\n");
    printf("%-6s %-12s %-18s %s\n", "id", "symbol", "address", "hits");
    for (int i = 0; i < npins; i++)
        printf("%-6llu %-12s 0x%-16lx %llu\n",
               (unsigned long long)pins[i].id, pins[i].sym,
               (unsigned long)pins[i].addr,
               (unsigned long long)pins[i].hits);

    printf("\n=== F7: 多线程安全 ===\n");
    printf("目标进程: %s (DONE=%s, 线程完成=%d/%d)\n",
           done ? "正常退出" : "异常/超时", done ? "yes" : "no",
           iters_received, nthreads);
    printf("事件总数: %llu\n", (unsigned long long)total_events);
    printf("事件涉及的独立线程数: %d\n", n_tids);
    printf("环形缓冲溢出计数: %llu\n", (unsigned long long)ludp_overflow(ctx));
    printf("%-4s %-18s %s\n", "#", "tid(tls base)", "events");
    for (int k = 0; k < n_tids; k++)
        printf("%-4d 0x%-16llx %llu\n", k,
               (unsigned long long)tid_counts[k][0],
               (unsigned long long)tid_counts[k][1]);
    printf("%-4s %-18s %s\n", "", "per-thread iters", "");
    for (int i = 0; i < nthreads; i++)
        printf("T%-3d %-18s %lu\n", i, "", th_iters[i]);

    free(ev);
    fprintf(stderr, "[demo] detaching\n");
    ludp_detach(ctx);
    kill(c.pid, SIGKILL);
    waitpid(c.pid, NULL, 0);
    close(c.out_fd); close(c.in_fd);
    return done && iters_received == nthreads ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* bench mode                                                        */
/* ------------------------------------------------------------------ */
static long run_bench_once(const char *target_path, long iters, bool probe)
{
    char tbuf[16];
    snprintf(tbuf, sizeof tbuf, "%ld", iters);
    child_t c;
    if (spawn_target(&c, target_path, "bench", tbuf, NULL) != 0)
        return -1;

    lr_t lr;
    lr_init(&lr, c.out_fd);
    char line[512];

    pid_t pid = 0;
    uintptr_t gp = 0, go_addr = 0;
    for (int i = 0; i < 5000; i++) {
        int n = lr_get(&lr, line, sizeof line, 20);
        if (n < 0) { fprintf(stderr, "[bench] no READY/ADDR/GO\n"); return -1; } /* EOF only */
        if (n == 0) continue;                                                     /* timeout: keep waiting */
        if (strncmp(line, "READY", 5) == 0) pid = (pid_t)atoi(line + 6);
        else if (sscanf(line, "ADDR getpid 0x%lx", &gp) == 1) { }
        else if (sscanf(line, "GO 0x%lx", &go_addr) == 1) { }
        if (pid && gp && go_addr) break;
    }
    fprintf(stderr, "[bench] pid=%d gp=0x%lx go=0x%lx\n",
            (int)pid, (unsigned long)gp, (unsigned long)go_addr);

    ludp_ctx *ctx = ludp_attach(pid);
    if (!ctx) { fprintf(stderr, "[bench] attach fail\n"); return -1; }
    fprintf(stderr, "[bench] attached\n");
    if (probe) {
        u64 id = 0;
        ludp_probe_add_addr(ctx, "getpid", gp, &id);
    }

    /* start the target by writing its user-space go flag, then resume it */
    {
        int one = 1;
        ludp_mem_write(ctx, go_addr, &one, sizeof one);
        ludp_resume(ctx);
    }

    long ns = -1;
    time_t t0 = time(NULL);
    for (;;) {
        int n = lr_get(&lr, line, sizeof line, 50);
        if (n < 0) { fprintf(stderr, "[bench] no BENCH_NS\n"); break; } /* EOF */
        if (n == 0) { if (time(NULL) - t0 > 120) break; continue; }     /* timeout: keep waiting */
        if (sscanf(line, "BENCH_NS %ld", &ns) == 1)
            break;
    }
    fprintf(stderr, "[bench] BENCH_NS=%ld\n", ns);

    /* wait for DONE so the child can exit cleanly */
    for (;;) {
        int n = lr_get(&lr, line, sizeof line, 50);
        if (n < 0) break;
        if (n == 0) { if (time(NULL) - t0 > 130) break; continue; }
        if (strncmp(line, "DONE", 4) == 0) break;
    }

    if (ctx) ludp_detach(ctx);
    waitpid(c.pid, NULL, 0);
    close(c.out_fd); close(c.in_fd);
    return ns;
}

static int run_bench(const char *target_path, long iters)
{
    printf("\n=== 基准: getpid 循环 %ld 次 ===\n", iters);
    long t0 = run_bench_once(target_path, iters, false);
    long t1 = run_bench_once(target_path, iters, true);
    if (t0 < 0 || t1 < 0) {
        fprintf(stderr, "bench failed\n");
        return 1;
    }
    double base_ns = (double)t0 / iters;
    double probe_ns = (double)t1 / iters;
    double overhead = (double)(t1 - t0) / iters;

    printf("%-22s %18s\n", "指标", "数值");
    printf("%-22s %18.2f ns/次\n", "无探针 (baseline)", base_ns);
    printf("%-22s %18.2f ns/次\n", "用户态探针", probe_ns);
    printf("%-22s %18.2f ns/次\n", "单次探针开销", overhead);
    printf("%-22s %18s\n", "阈值要求", "< 1000 ns/次");
    printf("%-22s %18s\n", "结论", overhead < 1000 ? "达标" : "未达标");
    return 0;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "stress";
    const char *target = "./target";

    if (strcmp(mode, "bench") == 0) {
        long iters = argc > 2 ? atol(argv[2]) : 1000000;
        return run_bench(target, iters);
    }

    int nthreads = argc > 2 ? atoi(argv[2]) : 4;
    int seconds = argc > 3 ? atoi(argv[3]) : 8;
    return run_stress(target, nthreads, seconds);
}
