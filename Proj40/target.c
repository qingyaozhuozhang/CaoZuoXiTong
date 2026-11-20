/*
 * target.c - demo tracee.
 *
 *   target stress <nthreads> <seconds>
 *       spawn nthreads workers that hammer a set of libc/libm functions,
 *       print per-thread iteration counts, then DONE.
 *
 *   target bench <iters>
 *       single-thread loop calling getpid() iters times, timed with
 *       CLOCK_MONOTONIC, prints BENCH_NS.
 *
 * Both modes print "READY <pid>" plus "ADDR <sym> 0x<addr>" lines (addresses
 * come from dlsym so the harness can cross-check its own ELF resolution),
 * then block on stdin until the harness sends the GO byte.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <math.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>

static atomic_int g_go = 0, g_stop = 0;
static unsigned long g_thread_iters[128];

typedef long   (*fn_i)(void);
typedef int    (*fn_ii)(void);
typedef void * (*fn_malloc)(size_t);
typedef void   (*fn_free)(void *);
typedef void * (*fn_calloc)(size_t, size_t);
typedef void * (*fn_realloc)(void *, size_t);
typedef int    (*fn_rand)(void);
typedef void   (*fn_srand)(unsigned int);
typedef void   (*fn_qsort)(void *, size_t, size_t, int (*)(const void *, const void *));
typedef char * (*fn_getenv)(const char *);
typedef int    (*fn_abs)(int);
typedef long   (*fn_labs)(long);
typedef int    (*fn_atoi)(const char *);
typedef double (*fn_d1)(double);
typedef double (*fn_d2)(double, double);

static fn_i      g_getpid, g_getppid, g_getuid, g_geteuid, g_getgid, g_getegid, g_gettid;
static fn_malloc g_malloc;  static fn_free    g_free;
static fn_calloc g_calloc;  static fn_realloc g_realloc;
static fn_rand   g_rand;    static fn_srand   g_srand;
static fn_qsort  g_qsort;   static fn_getenv  g_getenv;
static fn_abs    g_abs;     static fn_labs    g_labs;
static fn_atoi   g_atoi;
static fn_d1     g_sqrt, g_log, g_exp, g_log2, g_exp2, g_fabs, g_cbrt;
static fn_d2     g_hypot, g_pow, g_atan2, g_fmod;

static int intcmp(const void *a, const void *b)
{
    return (int)(*(const unsigned char *)a) - (int)(*(const unsigned char *)b);
}

static void *worker(void *arg)
{
    intptr_t id = (intptr_t)arg;
    unsigned long iters = 0;
    char buf[16] = "hello-probe";
    while (!atomic_load_explicit(&g_go, memory_order_acquire)) { }

    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        (void)g_getpid(); (void)g_getppid(); (void)g_getuid(); (void)g_geteuid();
        (void)g_getgid(); (void)g_getegid(); (void)g_gettid();

        void *m = g_malloc(64); g_free(m);
        void *ca = g_calloc(16, 4); g_free(ca);
        void *r = g_realloc(NULL, 32); g_free(r);

        (void)g_rand(); g_srand((unsigned)iters);
        g_qsort(buf, 5, 1, intcmp);
        (void)g_getenv("PATH");
        (void)g_abs(-5); (void)g_labs(-6L); (void)g_atoi("42");

        volatile double d = 0.5;
        d += g_sqrt(2.0);
        d += g_log(2.0);
        d += g_exp(1.0);
        d += g_log2(8.0);
        d += g_exp2(3.0);
        d += g_fabs(-1.0);
        d += g_cbrt(8.0);
        d += g_hypot(3.0, 4.0);
        d += g_pow(2.0, 10.0);
        d += g_atan2(1.0, 1.0);
        d += g_fmod(5.0, 2.0);
        if (d > 1e300) { /* never, but keeps the values live */
            atomic_fetch_add_explicit(&g_stop, 1, memory_order_relaxed);
        }
        iters++;
    }
    g_thread_iters[id] = iters;
    return NULL;
}

/* Register / flags / FP integrity target (defined in libctx.so). */
extern long ctx_check(int a, int b, int c, int d, int e, int f);
/* signal-safety function (defined in libctx.so). */
extern long sig_check(int a, int b);

static volatile long g_sig_count = 0;
static void sig_handler(int sig) { (void)sig; g_sig_count++; }

#define LOAD(dst, sym) do { (dst) = (__typeof__(dst))dlsym(RTLD_DEFAULT, sym); } while (0)

static void load_all(void)
{
    LOAD(g_getpid, "getpid");   LOAD(g_getppid, "getppid");
    LOAD(g_getuid, "getuid");   LOAD(g_geteuid, "geteuid");
    LOAD(g_getgid, "getgid");   LOAD(g_getegid, "getegid");
    LOAD(g_gettid, "gettid");
    LOAD(g_malloc, "malloc");   LOAD(g_free, "free");
    LOAD(g_calloc, "calloc");   LOAD(g_realloc, "realloc");
    LOAD(g_rand, "rand");       LOAD(g_srand, "srand");
    LOAD(g_qsort, "qsort");     LOAD(g_getenv, "getenv");
    LOAD(g_abs, "abs");         LOAD(g_labs, "labs");
    LOAD(g_atoi, "atoi");
    LOAD(g_sqrt, "sqrt");       LOAD(g_log, "log");
    LOAD(g_exp, "exp");         LOAD(g_log2, "log2");
    LOAD(g_exp2, "exp2");       LOAD(g_fabs, "fabs");
    LOAD(g_cbrt, "cbrt");       LOAD(g_hypot, "hypot");
    LOAD(g_pow, "pow");         LOAD(g_atan2, "atan2");
    LOAD(g_fmod, "fmod");
}

#define ADDRLINE(sym, var) \
    printf("ADDR %s 0x%lx\n", sym, (unsigned long)(void *)(var))

static void print_addrs(void)
{
    ADDRLINE("getpid", g_getpid);   ADDRLINE("getppid", g_getppid);
    ADDRLINE("getuid", g_getuid);   ADDRLINE("geteuid", g_geteuid);
    ADDRLINE("getgid", g_getgid);   ADDRLINE("getegid", g_getegid);
    ADDRLINE("gettid", g_gettid);
    ADDRLINE("malloc", g_malloc);   ADDRLINE("free", g_free);
    ADDRLINE("calloc", g_calloc);   ADDRLINE("realloc", g_realloc);
    ADDRLINE("rand", g_rand);       ADDRLINE("srand", g_srand);
    ADDRLINE("qsort", g_qsort);     ADDRLINE("getenv", g_getenv);
    ADDRLINE("abs", g_abs);         ADDRLINE("labs", g_labs);
    ADDRLINE("atoi", g_atoi);
    ADDRLINE("sqrt", g_sqrt);       ADDRLINE("log", g_log);
    ADDRLINE("exp", g_exp);         ADDRLINE("log2", g_log2);
    ADDRLINE("exp2", g_exp2);       ADDRLINE("fabs", g_fabs);
    ADDRLINE("cbrt", g_cbrt);       ADDRLINE("hypot", g_hypot);
    ADDRLINE("pow", g_pow);         ADDRLINE("atan2", g_atan2);
    ADDRLINE("fmod", g_fmod);
}

/* Spin on g_go in user space (no syscall in progress) so the tracer can
 * attach + remote-mmap safely; the tracer starts us by writing g_go = 1. */
static void handshake_go(void)
{
    printf("GO 0x%lx\n", (unsigned long)(uintptr_t)&g_go);
    while (!atomic_load_explicit(&g_go, memory_order_acquire)) { }
}

static long bench_getpid(long iters)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    volatile long acc = 0;
    for (long i = 0; i < iters; i++)
        acc += (long)g_getpid();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
}

/* A function that lives in the *main executable* (PIE, ~0x55..), deliberately
 * NOT in a shared library, so we can validate probing an out-of-pool target. */
static __attribute__((noinline)) long pie_probe_fn(int a)
{
    __asm__ volatile("" ::: "memory");
    return (long)a * 3 + 7;
}

static long bench_pie(long iters)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    volatile long acc = 0;
    for (long i = 0; i < iters; i++)
        acc += pie_probe_fn((int)i);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "stress";

    if (strcmp(mode, "bench") == 0) {
        long iters = argc > 2 ? atol(argv[2]) : 1000000;
        LOAD(g_getpid, "getpid");
        printf("READY %d\n", getpid());
        printf("ADDR getpid 0x%lx\n", (unsigned long)(void *)g_getpid);
        handshake_go();
        long ns = bench_getpid(iters);
        printf("BENCH_NS %ld\n", ns);
        printf("BENCH_ITERS %ld\n", iters);
        printf("DONE\n");
        struct timespec bye = { 1, 0 };
        nanosleep(&bye, NULL);   /* give the tracer time to restore + detach */
        return 0;
    }

    if (strcmp(mode, "pie") == 0) {
        long iters = argc > 2 ? atol(argv[2]) : 200000;
        printf("READY %d\n", getpid());
        printf("ADDR pie_probe_fn 0x%lx\n", (unsigned long)(void *)pie_probe_fn);
        handshake_go();
        long ns = bench_pie(iters);
        volatile long sum = 0;
        for (long i = 0; i < iters; i++)
            sum += pie_probe_fn((int)i);
        printf("PIE_SUM %ld\n", sum);
        printf("PIE_NS %ld\n", ns);
        printf("DONE\n");
        struct timespec bye = { 1, 0 };
        nanosleep(&bye, NULL);
        return 0;
    }

    if (strcmp(mode, "ctx") == 0) {
        long iters = argc > 2 ? atol(argv[2]) : 1000000;
        printf("READY %d\n", getpid());
        printf("ADDR ctx_check 0x%lx\n", (unsigned long)(void *)ctx_check);
        handshake_go();
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        long sum = 0;
        for (long i = 0; i < iters; i++)
            sum += ctx_check(1, 2, 3, 4, 5, 6);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ns = (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
        printf("CTX_SUM %ld\n", sum);
        printf("CTX_NS %ld\n", ns);
        printf("DONE\n");
        struct timespec bye = { 1, 0 };
        nanosleep(&bye, NULL);
        return 0;
    }

    if (strcmp(mode, "sig") == 0) {
        long iters = argc > 2 ? atol(argv[2]) : 1000000;
        signal(SIGUSR1, sig_handler);
        signal(SIGALRM, sig_handler);
        printf("READY %d\n", getpid());
        printf("ADDR sig_check 0x%lx\n", (unsigned long)(void *)sig_check);
        handshake_go();
        long sum = 0;
        for (long i = 0; i < iters; i++)
            sum += sig_check(2, 3);
        printf("SIG_COUNT %ld\n", g_sig_count);
        printf("SIG_SUM %ld\n", sum);
        printf("DONE\n");
        struct timespec bye = { 1, 0 };
        nanosleep(&bye, NULL);
        return 0;
    }

    int nthreads = argc > 2 ? atoi(argv[2]) : 4;
    int seconds = argc > 3 ? atoi(argv[3]) : 8;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 64) nthreads = 64;

    load_all();
    printf("READY %d\n", getpid());
    print_addrs();
    handshake_go();

    pthread_t th[64];
    for (int i = 0; i < nthreads; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);

    struct timespec ts = { seconds, 0 };
    nanosleep(&ts, NULL);
    atomic_store_explicit(&g_stop, 1, memory_order_release);

    unsigned long total = 0;
    for (int i = 0; i < nthreads; i++) {
        pthread_join(th[i], NULL);
        printf("ITER %d %lu\n", i, g_thread_iters[i]);
        total += g_thread_iters[i];
    }
    printf("TOTAL %lu\n", total);
    printf("DONE\n");
    struct timespec bye = { 1, 0 };
    nanosleep(&bye, NULL);   /* give the tracer time to restore + detach */
    return 0;
}
