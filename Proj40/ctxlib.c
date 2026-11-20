/* ctxlib.c - register/flags/FP + signal integrity targets, placed in a shared
 * library so they live in the 0x7f.. region (reachable by the trampoline pool),
 * exactly like the libc/libm functions F1/F6 probe. */
#include <signal.h>

__attribute__((noinline, used, visibility("default")))
long ctx_check(int a, int b, int c, int d, int e, int f)
{
    long s = (long)a + (long)b * 2 + (long)c * 3 + (long)d * 5 +
             (long)e * 7 + (long)f * 11;
    volatile double x = (double)a * 1.5 + (double)b;
    volatile double y = (double)c * 0.25 - (double)d;
    s += (long)(x * y);
    s += (e > f) ? (long)e : (long)f;
    __asm__ volatile ("" ::: "rbx", "rbp", "r12", "r13", "r14", "r15");
    return s;
}

__attribute__((noinline, used, visibility("default")))
long sig_check(int a, int b)
{
    return (long)a * 31 + (long)b;
}
