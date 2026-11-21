#include <stdint.h>
#include <signal.h>
#include <unistd.h>

__attribute__((noinline)) uint64_t six(uint64_t a, uint64_t b, uint64_t c,
                                    uint64_t d, uint64_t e, uint64_t f)
{ return a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13; }
__attribute__((noinline)) long negative(long n) { return -n - 7; }
__attribute__((noinline)) void *pointer(void *p) { return p; }
__attribute__((noinline)) double floating(double x) { return x * 1.5 + 0.25; }
__attribute__((noinline)) unsigned __int128 wide(uint64_t x)
{ return ((unsigned __int128)(x + 1) << 64) | x; }
__attribute__((noinline)) long stackargs(long a, long b, long c, long d,
                                      long e, long f, long g, long h)
{ return a + b + c + d + e + f + g * 10 + h * 100; }
__attribute__((noinline)) long recursive(long n)
{ if (!n) return 1; volatile long r = recursive(n - 1); return r + n; }
__attribute__((noinline)) long signalled(long n)
{ if (n) raise(SIGUSR1); return n + 42; }
