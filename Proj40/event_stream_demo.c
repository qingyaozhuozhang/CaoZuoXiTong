#define _GNU_SOURCE
/* A3 collector: real libc getpid events, on one known CPU, with an explicit
 * ready/go handshake so the external ftrace collector cannot miss startup. */
#include "ludp.h"
#include "validation_arch.h"
#include <errno.h>
#include <sched.h>
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

typedef struct { atomic_int ready, go, done, release, error; } control_t;

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

int main(int argc, char **argv)
{
    char *end = NULL;
    int standalone = argc == 2 && !strcmp(argv[1], "--standalone");
    long calls = standalone ? 8 : argc == 2 ? strtol(argv[1], &end, 10) : 100;
    if (argc > 2 || (argc == 2 && !standalone && (!end || *end)) || calls < 1 || calls > 512) {
        fputs("usage: event_stream_demo [calls:1..512 | --standalone]\n", stderr);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    cpu_set_t allowed, selected;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof allowed, &allowed)) {
        perror("sched_getaffinity");
        return 1;
    }
    int cpu = -1;
    for (int i = 0; i < CPU_SETSIZE; ++i)
        if (CPU_ISSET(i, &allowed)) { cpu = i; break; }
    if (cpu < 0) return 1;
    CPU_ZERO(&selected);
    CPU_SET(cpu, &selected);
    control_t *ctl = mmap(NULL, sizeof(*ctl), PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED) return 1;
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) { munmap(ctl, sizeof(*ctl)); return 1; }
    if (!child) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent ||
            sched_setaffinity(0, sizeof selected, &selected)) _exit(1);
        pid_t expected_pid = getpid();
        atomic_store(&ctl->ready, 1);
        while (!atomic_load(&ctl->go)) validation_cpu_relax();
        for (long i = 0; i < calls; ++i) {
            if (getpid() != expected_pid) atomic_store(&ctl->error, 1);
            /* Separate calls for an unambiguous kernel/user event pairing. */
            struct timespec gap = {.tv_nsec = 1000000};
            while (nanosleep(&gap, &gap) && errno == EINTR) {}
        }
        atomic_store(&ctl->done, 1);
        while (!atomic_load(&ctl->release)) validation_cpu_relax();
        _exit(atomic_load(&ctl->error) != 0);
    }

    int failed = 0, status = 0, stops = 0, n_events = 0;
    uint64_t begin = 0, finish = 0;
    ludp_ctx *ctx = NULL;
    u64 id = 0;
    ludp_event events[1024];
    ludp_probe_info info;
    uint64_t deadline = now_ns() + UINT64_C(10000000000);
    while (!atomic_load(&ctl->ready) && now_ns() < deadline) usleep(100);
    if (!atomic_load(&ctl->ready)) { failed = 1; goto cleanup; }
    ctx = ludp_attach(child);
    if (!ctx || ludp_probe_add_ex(ctx, "libc.so", "getpid",
            LUDP_PROBE_ENTRY | LUDP_PROBE_RETURN, &id) ||
        ludp_probe_list(ctx, &info, 1) != 1 || ludp_resume(ctx)) {
        perror("attach/install"); failed = 1; goto cleanup;
    }
    printf("READY pid=%ld cpu=%d calls=%ld\n", (long)child, cpu, calls);
    char command[16];
    if (!standalone && (!fgets(command, sizeof command, stdin) || strcmp(command, "GO\n"))) {
        fputs("collector cancelled before GO\n", stderr);
        failed = 1; goto cleanup;
    }
    begin = now_ns();
    atomic_store(&ctl->go, 1);
    deadline = begin + UINT64_C(10000000000);
    while (!atomic_load(&ctl->done) && now_ns() < deadline) {
        int count = ludp_poll(ctx);
        if (count < 0) { failed = 1; goto cleanup; }
        stops += count;
        usleep(100);
    }
    finish = now_ns();
    if (!atomic_load(&ctl->done) || atomic_load(&ctl->error)) {
        failed = 1; goto cleanup;
    }
    for (;;) {
        int count = 0;
        if (ludp_events_read(ctx, events + n_events, 1024 - n_events, &count)) {
            failed = 1; goto cleanup;
        }
        n_events += count;
        if (!count || n_events == 1024) break;
    }
    if (n_events != calls * 2 || ludp_overflow(ctx) ||
        ludp_return_missed(ctx) || stops) { failed = 1; goto cleanup; }
    for (int i = 0; i < n_events; ++i) {
        char line[512];
        if (events[i].kernel_tid != (u64)child ||
            events[i].type != (i % 2 ? LUDP_EVENT_RETURN : LUDP_EVENT_ENTRY) ||
            !events[i].call_id ||
            (i % 2 && (events[i].call_id != events[i - 1].call_id ||
                       events[i].retval != (u64)child)) ||
            ludp_event_format_perf(&events[i], &info, child, cpu, "ludp-getpid",
                                   line, sizeof line) < 0) {
            failed = 1; goto cleanup;
        }
        fputs(line, stdout);
    }
    printf("SUMMARY {\"pid\":%ld,\"cpu\":%d,\"calls\":%ld,\"events\":%d,"
           "\"begin_ns\":%llu,\"end_ns\":%llu,\"overflow\":0,"
           "\"return_missed\":0,\"runtime_stops\":%d}\n",
           (long)child, cpu, calls, n_events, (unsigned long long)begin,
           (unsigned long long)finish, stops);
cleanup:
    if (ctx && ludp_detach(ctx)) { perror("ludp_detach"); failed = 1; }
    if (failed) kill(child, SIGKILL); /* Only the child created by this demo. */
    else atomic_store(&ctl->release, 1);
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    munmap(ctl, sizeof(*ctl));
    return failed;
}
