#define _GNU_SOURCE
#include "ludp.h"
#include "validation_arch.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <time.h>
#include <dlfcn.h>

typedef struct { atomic_int ready, go, done, release; uint64_t ns, sum; } shared;
static uint64_t ns_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }

/* Linux uprobe PMU uses an object path and file offset, not a virtual address.
 * Each sample includes IP/TID. Counting still works if the perf sample ring
 * fills; read(fd) verifies how many kernel probe executions actually occurred. */
static const char *kernel_probe_stage;

static int kernel_probe(pid_t pid)
{
    kernel_probe_stage="read_uprobe_pmu_type";
    FILE *f=fopen("/sys/bus/event_source/devices/uprobe/type","r");
    unsigned type=0;
    if(!f) return -1;
    int n=fscanf(f,"%u",&type); fclose(f);
    if(n!=1) { errno=ENODEV; return -1; }
    kernel_probe_stage="resolve_getpid_mapping";
    uintptr_t addr=(uintptr_t)dlsym(RTLD_DEFAULT,"getpid");
    f=fopen("/proc/self/maps","r");
    if(!f) return -1;
    char line[1024], path[768]=""; unsigned long lo,hi,off; int pos;
    uint64_t offset=0;
    while(fgets(line,sizeof line,f)) {
        if(sscanf(line,"%lx-%lx %*s %lx %*s %*s %n",&lo,&hi,&off,&pos)==3 && addr>=lo && addr<hi) {
            snprintf(path,sizeof path,"%s",line+pos); path[strcspn(path,"\n")]=0;
            offset=off+addr-lo; break;
        }
    }
    fclose(f);
    if(!path[0]) { errno=ENOENT; return -1; }
    struct perf_event_attr a={0};
    a.size=sizeof a; a.type=type; a.config1=(uintptr_t)path; a.config2=offset;
    a.sample_period=1; a.sample_type=PERF_SAMPLE_IP|PERF_SAMPLE_TID;
    a.disabled=1; a.wakeup_events=1;
    kernel_probe_stage="perf_event_open";
    return syscall(__NR_perf_event_open,&a,pid,-1,-1,PERF_FLAG_FD_CLOEXEC);
}

static void kernel_diagnostic(int error)
{
    int paranoid=0;
    FILE *f=fopen("/proc/sys/kernel/perf_event_paranoid","r");
    int have_policy=f && fscanf(f,"%d",&paranoid)==1;
    if(f) fclose(f);
    printf("DIAGNOSTIC uprobe stage=%s errno=%d euid=%u perf_event_paranoid=",
           kernel_probe_stage,error,(unsigned)geteuid());
    if(have_policy) printf("%d",paranoid); else printf("unavailable");
    printf("\n");
    if(error==EPERM || error==EACCES)
        printf("DIAGNOSTIC uprobe requires permission to use perf_event_open; check CAP_PERFMON, perf_event_paranoid and the runner security policy.\n");
}

static int run(const char *mode, long iters, int round)
{
    shared *s=mmap(NULL,sizeof *s,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(s==MAP_FAILED) return 1;
    pid_t pid=fork();
    if(pid==0) {
        for(int i=0;i<10000;i++) (void)getpid();
        atomic_store(&s->ready,1);
        while(!atomic_load(&s->go)) validation_cpu_relax();
        uint64_t sum=0,start=ns_now();
        for(long i=0;i<iters;i++) sum+=(unsigned)getpid();
        s->ns=ns_now()-start; s->sum=sum;
        atomic_store(&s->done,1);
        while(!atomic_load(&s->release)) validation_cpu_relax();
        _exit(0);
    }
    if(pid<0) { munmap(s,sizeof *s); return 1; }
    ludp_ctx *ctx=NULL; u64 id=0, events=0, dropped=0, missed=0;
    uint64_t attach_ns=0, add_ns=0, remove_ns=0, detach_ns=0, cleanup_start=0, cleanup_ns=0;
    int failed=0, skipped=0, fd=-1, status=0, runtime_stops=0;
    uintptr_t gp=(uintptr_t)dlsym(RTLD_DEFAULT,"getpid");
    unsigned char original[32], observed[32];
    memcpy(original,(void *)gp,sizeof original);
    void *perf_ring=MAP_FAILED; size_t perf_size=9*(size_t)sysconf(_SC_PAGESIZE);
    uint64_t limit=ns_now()+30000000000ULL;
    while(!atomic_load(&s->ready) && ns_now()<limit) usleep(100);
    if(!atomic_load(&s->ready)) { failed=1; goto cleanup; }
    if(!strcmp(mode,"uprobe")) {
        fd=kernel_probe(pid);
        if(fd<0) {
            int error=errno;
            printf("SKIP uprobe: %s\n",strerror(error));
            kernel_diagnostic(error); skipped=1; goto cleanup;
        }
        kernel_probe_stage="mmap_perf_ring";
        perf_ring=mmap(NULL,perf_size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(perf_ring==MAP_FAILED) { kernel_diagnostic(errno); failed=1; goto cleanup; }
        kernel_probe_stage="enable_perf_event";
        if(ioctl(fd,PERF_EVENT_IOC_ENABLE,0)) { kernel_diagnostic(errno); failed=1; goto cleanup; }
    } else if(strcmp(mode,"baseline")) {
        uint64_t start=ns_now(); ctx=ludp_attach(pid); attach_ns=ns_now()-start;
        if(!ctx) { perror("attach"); failed=1; goto cleanup; }
        if(ludp_mem_read(ctx,gp,observed,sizeof observed) || memcmp(original,observed,sizeof original)) {
            failed=1; goto cleanup;
        }
        unsigned flags=LUDP_PROBE_ENTRY;
        if(!strcmp(mode,"return")) flags|=LUDP_PROBE_RETURN;
        if(strcmp(mode,"inert")) {
            start=ns_now();
            if(ludp_probe_add_ex(ctx,"libc.so.6","getpid",flags,&id)) { failed=1; goto cleanup; }
            add_ns=ns_now()-start;
            if(ludp_mem_read(ctx,gp,observed,VALIDATION_ENTRY_READ_SIZE) || !validation_entry_is_branch(observed)) { failed=1; goto cleanup; }
        }
        if(ludp_resume(ctx)) { failed=1; goto cleanup; }
    }
    atomic_store(&s->go,1);
    while(!atomic_load(&s->done) && ns_now()<limit) {
        if(ctx) { int stops=ludp_poll(ctx); if(stops<0) { failed=1; break; } runtime_stops+=stops; }
        usleep(100);
    }
    if(!atomic_load(&s->done) || s->sum!=(uint64_t)pid*iters || !s->ns) { failed=1; goto cleanup; }
    if(runtime_stops) failed=1;
    if(fd>=0) {
        if(ioctl(fd,PERF_EVENT_IOC_DISABLE,0)) failed=1;
        if(read(fd,&events,sizeof events)!=sizeof events || events!=(u64)iters) failed=1;
    }
    if(ctx) {
        ludp_event ev[4096]; int n;
        do {
            if(ludp_events_read(ctx,ev,4096,&n)) { failed=1; break; }
            for(int i=0;i<n;i++) {
                if(ev[i].probe_id!=id || (ev[i].type==LUDP_EVENT_RETURN && ev[i].retval!=(u64)pid)) failed=1;
            }
            events+=n;
        } while(n);
        dropped=ludp_overflow(ctx); missed=ludp_return_missed(ctx);
        u64 expected=!strcmp(mode,"inert")?0:(u64)iters*(!strcmp(mode,"return")?2:1);
        if(events!=expected || dropped || missed) failed=1;
        if(id) {
            uint64_t start=ns_now(); cleanup_start=start;
            if(ludp_probe_remove(ctx,id)) failed=1;
            remove_ns=ns_now()-start;
        }
        if(ludp_mem_read(ctx,gp,observed,sizeof observed) || memcmp(original,observed,sizeof original)) failed=1;
    }
cleanup:
    if(ctx) {
        uint64_t start=ns_now();
        if(!cleanup_start) cleanup_start=start;
        if(ludp_detach(ctx)) failed=1;
        uint64_t finished=ns_now();
        detach_ns=finished-start; cleanup_ns=finished-cleanup_start;
    }
    if(perf_ring!=MAP_FAILED) munmap(perf_ring,perf_size);
    if(fd>=0) close(fd);
    if(failed || skipped) kill(pid,SIGKILL); else atomic_store(&s->release,1);
    pid_t waited;
    do { waited=waitpid(pid,&status,0); } while(waited<0 && errno==EINTR);
    if(waited!=pid) failed=1;
    if(!skipped && (!WIFEXITED(status) || WEXITSTATUS(status))) failed=1;
    if(!skipped) printf("SAMPLE mode=%s round=%d iterations=%ld total_ns=%lu ns_per_call=%.3f events=%lu drops=%lu missed=%lu stops=%d attach_ms=%.3f insert_ms=%.3f remove_ms=%.3f detach_ms=%.3f cleanup_ms=%.3f status=%s\n",
        mode,round,iters,s->ns,(double)s->ns/iters,events,dropped,missed,runtime_stops,attach_ns/1e6,add_ns/1e6,remove_ns/1e6,detach_ns/1e6,cleanup_ns/1e6,failed?"FAIL":"PASS");
    munmap(s,sizeof *s);
    return skipped?77:failed;
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--kernel-check")) {
        setvbuf(stdout,NULL,_IONBF,0);
        return run("uprobe",1000,1);
    }
    long iters=argc>1?strtol(argv[1],NULL,10):1000000;
    int rounds=argc>2?atoi(argv[2]):5;
    if(iters<1 || iters>2000000 || rounds<1 || rounds>100) return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    const char *modes[]={"baseline","inert","entry","return","uprobe"};
    int failed=0;
    for(int r=0;r<rounds;r++) for(int k=0;k<5;k++) {
        int rc=run(modes[(k+r)%5],iters,r+1);
        if(rc!=77) failed|=rc;
    }
    return failed;
}
