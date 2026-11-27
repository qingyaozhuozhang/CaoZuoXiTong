#define _GNU_SOURCE
#include "ludp.h"
#include "validation_arch.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

enum { MAX_EVENTS = 64 };

typedef struct {
    atomic_int ready, phase, done, release, errors, callbacks;
    atomic_int block_callback, callback_entered;
    atomic_int clobber_avx;
    u64 expected_probe_id;
    u64 observed_probe_id;
    u64 observed_args[6];
    u64 observed_user_data;
} control_t;

#if defined(__x86_64__)
#define VECTOR_STATE_NAME "AVX"
static int vector_state_supported(void) { return __builtin_cpu_supports("avx"); }
extern uint64_t a4_vector_target(void) __asm__("a4_avx_target");
extern uint64_t a4_vector_invoke(void) __asm__("a4_avx_invoke");

__attribute__((target("avx"), noinline))
static void clobber_vector_state(void)
{
    __asm__ volatile(
        "vpxor %%ymm0, %%ymm0, %%ymm0\n\t"
        "vpxor %%ymm1, %%ymm1, %%ymm1\n\t"
        "vpxor %%ymm2, %%ymm2, %%ymm2\n\t"
        "vpxor %%ymm3, %%ymm3, %%ymm3\n\t"
        "vpxor %%ymm4, %%ymm4, %%ymm4\n\t"
        "vpxor %%ymm5, %%ymm5, %%ymm5\n\t"
        "vpxor %%ymm6, %%ymm6, %%ymm6\n\t"
        "vpxor %%ymm7, %%ymm7, %%ymm7\n\t"
        "vpxor %%ymm8, %%ymm8, %%ymm8\n\t"
        "vpxor %%ymm9, %%ymm9, %%ymm9\n\t"
        "vpxor %%ymm10, %%ymm10, %%ymm10\n\t"
        "vpxor %%ymm11, %%ymm11, %%ymm11\n\t"
        "vpxor %%ymm12, %%ymm12, %%ymm12\n\t"
        "vpxor %%ymm13, %%ymm13, %%ymm13\n\t"
        "vpxor %%ymm14, %%ymm14, %%ymm14\n\t"
        "vpxor %%ymm15, %%ymm15, %%ymm15"
        : : :
          "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
          "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15",
          "cc", "memory");
}

#else
#define VECTOR_STATE_NAME "ARM64 SIMD"
static int vector_state_supported(void) { return 1; }
extern uint64_t a4_vector_target(void);
extern uint64_t a4_vector_invoke(void);

__attribute__((noinline))
static void clobber_vector_state(void)
{
    __asm__ volatile(
        "movi v0.16b, #0\n\t"
        "movi v1.16b, #0\n\t"
        "movi v2.16b, #0\n\t"
        "movi v3.16b, #0\n\t"
        "movi v4.16b, #0\n\t"
        "movi v5.16b, #0\n\t"
        "movi v6.16b, #0\n\t"
        "movi v7.16b, #0\n\t"
        "movi v8.16b, #0\n\t"
        "movi v9.16b, #0\n\t"
        "movi v10.16b, #0\n\t"
        "movi v11.16b, #0\n\t"
        "movi v12.16b, #0\n\t"
        "movi v13.16b, #0\n\t"
        "movi v14.16b, #0\n\t"
        "movi v15.16b, #0\n\t"
        "movi v16.16b, #0\n\t"
        "movi v17.16b, #0\n\t"
        "movi v18.16b, #0\n\t"
        "movi v19.16b, #0\n\t"
        "movi v20.16b, #0\n\t"
        "movi v21.16b, #0\n\t"
        "movi v22.16b, #0\n\t"
        "movi v23.16b, #0\n\t"
        "movi v24.16b, #0\n\t"
        "movi v25.16b, #0\n\t"
        "movi v26.16b, #0\n\t"
        "movi v27.16b, #0\n\t"
        "movi v28.16b, #0\n\t"
        "movi v29.16b, #0\n\t"
        "movi v30.16b, #0\n\t"
        "movi v31.16b, #0\n\t"
        : : :
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
          "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
          "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "memory");
}

#endif

static uint64_t expected_integer(uint64_t a)
{
    return a + 2 * 3 + 3 * 5 + 4 * 7 + 5 * 11 + 6 * 13;
}

__attribute__((noinline, noclone))
static double a4_target(uint64_t a, uint64_t b, uint64_t c,
                        uint64_t d, uint64_t e, uint64_t f, double x)
{
#if defined(__x86_64__)
    unsigned short x87_cw;
    __asm__ volatile("fnstcw %0" : "=m"(x87_cw));
    if ((_mm_getcsr() & _MM_ROUND_MASK) != _MM_ROUND_NEAREST ||
        (x87_cw & 0x0c00) != 0)
        return -1.0;
#else
    uint64_t fpcr, fpsr;
    __asm__ volatile("mrs %0, fpcr\n\tmrs %1, fpsr" : "=r"(fpcr), "=r"(fpsr));
    if ((fpcr & (UINT64_C(3) << 22)) || (fpsr & 1))
        return -1.0;
#endif
    return (double)(a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13) + x;
}

__attribute__((noinline, noclone))
static void a4_callback(const ludp_call_context *context)
{
    control_t *ctl = (control_t *)(uintptr_t)context->user_data;
    if (!ctl)
        return;
    if (context->probe_id != ctl->expected_probe_id ||
        (!atomic_load(&ctl->clobber_avx) &&
         (context->args[1] != 2 || context->args[2] != 3 ||
          context->args[3] != 4 || context->args[4] != 5 ||
          context->args[5] != 6)))
        atomic_fetch_add(&ctl->errors, 1);
    ctl->observed_probe_id = context->probe_id;
    memcpy(ctl->observed_args, context->args, sizeof ctl->observed_args);
    ctl->observed_user_data = context->user_data;
    atomic_fetch_add(&ctl->callbacks, 1);
    if (atomic_load(&ctl->block_callback)) {
        atomic_store(&ctl->callback_entered, 1);
        while (atomic_load(&ctl->block_callback)) validation_cpu_relax();
    }

#if defined(__x86_64__)
    /* Deliberately destroy every SysV caller-saved GPR, all XMM registers,
     * RFLAGS, MXCSR rounding mode and the x87 control word. The trampoline
     * must restore the original target-call context after this returns. */
    unsigned mxcsr = 0x3f80;
    unsigned short x87_cw = 0x077f;
    __asm__ volatile(
        "ldmxcsr %0\n\t"
        "fldcw %1\n\t"
        "mov $0x11, %%rax\n\t"
        "mov $0x22, %%rcx\n\t"
        "mov $0x33, %%rdx\n\t"
        "mov $0x44, %%rsi\n\t"
        "mov $0x55, %%rdi\n\t"
        "mov $0x66, %%r8\n\t"
        "mov $0x77, %%r9\n\t"
        "mov $0x88, %%r10\n\t"
        "mov $0x99, %%r11\n\t"
        "pxor %%xmm0, %%xmm0\n\t"
        "pxor %%xmm1, %%xmm1\n\t"
        "pxor %%xmm2, %%xmm2\n\t"
        "pxor %%xmm3, %%xmm3\n\t"
        "pxor %%xmm4, %%xmm4\n\t"
        "pxor %%xmm5, %%xmm5\n\t"
        "pxor %%xmm6, %%xmm6\n\t"
        "pxor %%xmm7, %%xmm7\n\t"
        "pxor %%xmm8, %%xmm8\n\t"
        "pxor %%xmm9, %%xmm9\n\t"
        "pxor %%xmm10, %%xmm10\n\t"
        "pxor %%xmm11, %%xmm11\n\t"
        "pxor %%xmm12, %%xmm12\n\t"
        "pxor %%xmm13, %%xmm13\n\t"
        "pxor %%xmm14, %%xmm14\n\t"
        "pxor %%xmm15, %%xmm15\n\t"
        "cmp %%rax, %%rcx\n\t"
        : : "m"(mxcsr), "m"(x87_cw)
        : "rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11",
          "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
          "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
          "cc", "memory");
#else
    /* Exercise restoration of AAPCS64 argument/scratch registers, NZCV,
     * SIMD state, FPCR rounding mode and FPSR exception flags. */
    __asm__ volatile(
        "mov x0, #0x400000\n\t"
        "msr fpcr, x0\n\t"
        "mov x0, #1\n\t"
        "msr fpsr, x0\n\t"
        "mov x0, #17\n\t"
        "mov x1, #18\n\t"
        "mov x2, #19\n\t"
        "mov x3, #20\n\t"
        "mov x4, #21\n\t"
        "mov x5, #22\n\t"
        "mov x6, #23\n\t"
        "mov x7, #24\n\t"
        "mov x8, #25\n\t"
        "mov x9, #26\n\t"
        "mov x10, #27\n\t"
        "mov x11, #28\n\t"
        "mov x12, #29\n\t"
        "mov x13, #30\n\t"
        "mov x14, #31\n\t"
        "mov x15, #32\n\t"
        "mov x16, #33\n\t"
        "mov x17, #34\n\t"
        "cmp x0, x1\n\t"
        : : : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
              "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
              "x16", "x17", "cc", "memory");
    clobber_vector_state();
#endif
    if (atomic_load(&ctl->clobber_avx))
        clobber_vector_state();
}

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void run_calls(control_t *ctl, int phase, int begin, int end)
{
    while (atomic_load(&ctl->phase) != phase) validation_cpu_relax();
    for (int i = begin; i < end; ++i) {
        double x = (double)i + 0.25;
        double expected = (double)expected_integer((u64)i) + x;
        if (a4_target((u64)i, 2, 3, 4, 5, 6, x) != expected)
            atomic_fetch_add(&ctl->errors, 1);
    }
    atomic_store(&ctl->done, phase);
}

static void child_main(control_t *ctl)
{
    atomic_store(&ctl->ready, 1);
    run_calls(ctl, 1, 0, 10);
    run_calls(ctl, 2, 10, 15);
    run_calls(ctl, 3, 42, 43);
    run_calls(ctl, 4, 43, 44);
    while (atomic_load(&ctl->phase) != 5) validation_cpu_relax();
    if (atomic_load(&ctl->clobber_avx) &&
        a4_vector_invoke() != UINT64_C(0x0123456789abcdef))
        atomic_fetch_add(&ctl->errors, 1);
    atomic_store(&ctl->done, 5);
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

static int drain(ludp_ctx *ctx, ludp_event events[MAX_EVENTS])
{
    int total = 0;
    for (;;) {
        int n = 0;
        if (ludp_events_read(ctx, events + total, MAX_EVENTS - total, &n))
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

static int check_pairs(const ludp_event *events, int n, u64 id,
                       int first_arg, int count)
{
    if (n != count * 2)
        return -1;
    for (int i = 0; i < count; ++i) {
        const ludp_event *entry = &events[i * 2];
        const ludp_event *ret = &events[i * 2 + 1];
        double fp_ret = 0;
        memcpy(&fp_ret, ret->fp_ret, sizeof fp_ret);
        int arg = first_arg + i;
        double expected = (double)expected_integer((u64)arg) + (double)arg + 0.25;
        if (entry->type != LUDP_EVENT_ENTRY || ret->type != LUDP_EVENT_RETURN ||
            entry->probe_id != id || ret->probe_id != id ||
            entry->args[0] != (u64)arg || ret->args[0] != (u64)arg ||
            !entry->call_id || ret->call_id != entry->call_id || fp_ret != expected)
            return -1;
    }
    return 0;
}

static int get_info(ludp_ctx *ctx, u64 id, ludp_probe_info *out)
{
    ludp_probe_info list[4];
    int n = ludp_probe_list(ctx, list, 4);
    for (int i = 0; i < n; ++i) {
        if (list[i].id == id) {
            *out = list[i];
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
    memcpy(original, (const void *)(uintptr_t)a4_target, sizeof original);

    control_t *ctl = mmap(NULL, sizeof *ctl, PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED)
        return 1;
    pid_t child = fork();
    if (child < 0) {
        munmap(ctl, sizeof *ctl);
        return 1;
    }
    if (child == 0)
        child_main(ctl);

    uint64_t ready_deadline = monotonic_ns() + UINT64_C(10000000000);
    while (!atomic_load(&ctl->ready) && monotonic_ns() < ready_deadline) usleep(100);
    if (!atomic_load(&ctl->ready)) { failed = 1; goto finish; }
    ludp_ctx *ctx = ludp_attach(child);
    if (!ctx) {
        failed = 1;
        goto finish;
    }

    ludp_filter filter = {
        .op = LUDP_FILTER_GE,
        .arg_index = 0,
        .value = 5,
    };
    ludp_call_action call = {
        .function = (uintptr_t)a4_callback,
        .user_data = (u64)(uintptr_t)ctl,
    };
    u64 id = 0;
    if (ludp_probe_add_addr_call_ex(ctx, "a4_target", (uintptr_t)a4_target,
            LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN, &filter, &call, &id)) {
        failed = 1;
        goto cleanup_ctx;
    }
    ctl->expected_probe_id = id;
    if (ludp_resume(ctx)) {
        failed = 1;
        goto cleanup_ctx;
    }

    ludp_event events[MAX_EVENTS];
    atomic_store(&ctl->phase, 1);
    int n = wait_phase(ctx, ctl, 1) ? -1 : drain(ctx, events);
    if (n != 10 || check_pairs(events, n, id, 5, 5) ||
        atomic_load(&ctl->callbacks) != 5 || atomic_load(&ctl->errors) ||
        ctl->observed_probe_id != id || ctl->observed_args[0] != 9 ||
        ctl->observed_args[5] != 6 ||
        ctl->observed_user_data != (u64)(uintptr_t)ctl) {
        failed = 1;
        goto cleanup_ctx;
    }

    ludp_probe_info info, after_bad;
    ludp_probe_config config;
    if (get_info(ctx, id, &info) || info.generation != 1 ||
        info.call.function != call.function ||
        info.call.user_data != call.user_data ||
        ludp_probe_get_config(ctx, id, &config)) {
        failed = 1;
        goto cleanup_ctx;
    }

    ludp_call_action invalid = {.function = 1, .user_data = 7};
    errno = 0;
    if (ludp_probe_set_call(ctx, id, &invalid) == 0 || errno != EACCES ||
        get_info(ctx, id, &after_bad) || after_bad.generation != 1 ||
        memcmp(&after_bad.call, &call, sizeof call) != 0) {
        failed = 1;
        goto cleanup_ctx;
    }

    if (ludp_probe_set_call(ctx, id, NULL) ||
        get_info(ctx, id, &info) || info.generation != 2 || info.call.function) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 2);
    n = wait_phase(ctx, ctl, 2) ? -1 : drain(ctx, events);
    if (n != 10 || check_pairs(events, n, id, 10, 5) ||
        atomic_load(&ctl->callbacks) != 5 || atomic_load(&ctl->errors)) {
        failed = 1;
        goto cleanup_ctx;
    }

    config.filter = (ludp_filter){
        .op = LUDP_FILTER_EQ,
        .arg_index = 0,
        .value = 42,
    };
    config.call = call;
    if (ludp_probe_update(ctx, id, &config) ||
        get_info(ctx, id, &info) || info.generation != 3) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->phase, 3);
    n = wait_phase(ctx, ctl, 3) ? -1 : drain(ctx, events);
    if (n != 2 || check_pairs(events, n, id, 42, 1) ||
        atomic_load(&ctl->callbacks) != 6 || atomic_load(&ctl->errors) ||
        ludp_overflow(ctx) || ludp_return_missed(ctx))
        failed = 1;
    if (failed)
        goto cleanup_ctx;

    config.filter.value = 43;
    if (ludp_probe_update(ctx, id, &config) ||
        get_info(ctx, id, &info) || info.generation != 4) {
        failed = 1;
        goto cleanup_ctx;
    }
    atomic_store(&ctl->block_callback, 1);
    atomic_store(&ctl->phase, 4);
    uint64_t deadline = monotonic_ns() + UINT64_C(5000000000);
    while (!atomic_load(&ctl->callback_entered) && monotonic_ns() < deadline)
        usleep(100);
    errno = 0;
    if (!atomic_load(&ctl->callback_entered) ||
        ludp_probe_set_call(ctx, id, NULL) == 0 || errno != EBUSY ||
        get_info(ctx, id, &info) || info.generation != 4 ||
        info.call.function != call.function) {
        failed = 1;
        atomic_store(&ctl->block_callback, 0);
        goto cleanup_ctx;
    }
    atomic_store(&ctl->block_callback, 0);
    n = wait_phase(ctx, ctl, 4) ? -1 : drain(ctx, events);
    if (n != 2 || check_pairs(events, n, id, 43, 1) ||
        atomic_load(&ctl->callbacks) != 7 || atomic_load(&ctl->errors) ||
        ludp_probe_set_call(ctx, id, NULL) ||
        get_info(ctx, id, &info) || info.generation != 5 || info.call.function)
        failed = 1;
    if (failed || ludp_probe_remove(ctx, id)) {
        failed = 1;
        goto cleanup_ctx;
    }

    if (vector_state_supported()) {
        atomic_store(&ctl->clobber_avx, 1);
        call.function = (uintptr_t)a4_callback;
        if (ludp_probe_add_addr_call_ex(ctx, "a4_vector_target",
                (uintptr_t)a4_vector_target, LUDP_PROBE_ENTRY, NULL,
                &call, &id)) {
            failed = 1;
            goto cleanup_ctx;
        }
        ctl->expected_probe_id = id;
    }
    atomic_store(&ctl->phase, 5);
    n = wait_phase(ctx, ctl, 5) ? -1 : drain(ctx, events);
    if (n != (vector_state_supported() ? 1 : 0) ||
        atomic_load(&ctl->errors) ||
        (vector_state_supported() &&
         (events[0].type != LUDP_EVENT_ENTRY || events[0].probe_id != id ||
          atomic_load(&ctl->callbacks) != 8 || ludp_probe_remove(ctx, id))))
        failed = 1;
    if (!vector_state_supported())
        puts("SKIP A4 " VECTOR_STATE_NAME " register preservation: CPU/OS does not support AVX");
    else
        puts(failed ? "A4 " VECTOR_STATE_NAME " register preservation: FAIL" :
                      "A4 " VECTOR_STATE_NAME " register preservation: PASS");

cleanup_ctx:
    if (ctx && ludp_detach(ctx))
        failed = 1;
    else if (ctx)
        detached = true;
    if (detached && !failed) {
        struct iovec local = {actual, sizeof actual};
        struct iovec remote = {(void *)(uintptr_t)a4_target, sizeof actual};
        if (process_vm_readv(child, &local, 1, &remote, 1, 0) !=
                (ssize_t)sizeof actual || memcmp(actual, original, sizeof actual))
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
    munmap(ctl, sizeof *ctl);
    puts(failed ? "A4 in-process call suite: FAIL" :
                  "A4 in-process call suite: PASS");
    return failed;
}
