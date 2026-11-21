#define _GNU_SOURCE
#include "ludp_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <dlfcn.h>

#ifndef LUDP_EVENT_RETURN
int main(void) { fputs("FAIL: F3 return event/API not implemented\n", stderr); return 1; }
#else
extern int abi_exercise(void);
extern unsigned char abi_before[672], abi_after[672];
extern uint64_t six(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t);
extern long negative(long), recursive(long), signalled(long);
extern void *pointer(void *);
extern double floating(double);
extern unsigned __int128 wide(uint64_t);
extern long stackargs(long,long,long,long,long,long,long,long);
typedef struct { atomic_int go, done, release, errors; } shared;
static shared *s;
static volatile sig_atomic_t signals;
static void handler(int sig) { (void)sig; if (signalled(0) == 42) signals++; }
static void *worker(void *unused)
{
    (void)unused;
    for (int i=0; i<100; ++i) {
        if (six(1,2,3,4,5,0xfedcba9876543210ULL) !=
            1ULL+6+15+28+55+0xfedcba9876543210ULL*13)
            atomic_fetch_add(&s->errors,1);
    }
    return NULL;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int run(unsigned flags, int fallback)
{
    const char *names[] = {"six","negative","pointer","floating","wide","stackargs","recursive","signalled","abi_target"};
    u64 ids[9] = {0}, counts[9][2] = {{0}};
    ludp_event entries[2048] = {0};
    int failed=0, status=0;
    s=mmap(NULL,sizeof *s,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(s==MAP_FAILED) return 1;
    pid_t pid=fork();
    if(pid==0) {
        signal(SIGUSR1,handler);
        while(!atomic_load(&s->go)) __asm__ volatile("pause");
        pthread_t th[4];
        for(int i=0;i<4;i++) if(pthread_create(&th[i],NULL,worker,NULL)) _exit(2);
        for(int i=0;i<4;i++) pthread_join(th[i],NULL);
        if(negative(123)!=-130 || pointer(s)!=s || floating(2.5)!=4.0 ||
           wide(0xdeadbeef12345678ULL)!=(((unsigned __int128)0xdeadbeef12345679ULL<<64)|0xdeadbeef12345678ULL) ||
           stackargs(1,2,3,4,5,6,7,8)!=891 || recursive(8)!=37 || signalled(1)!=43 || signals!=1)
            atomic_fetch_add(&s->errors,1);
        if (abi_exercise() || memcmp(abi_before,abi_after,sizeof abi_before))
            atomic_fetch_add(&s->errors,1);
        atomic_store(&s->done,1);
        while(!atomic_load(&s->release)) __asm__ volatile("pause");
        _exit(atomic_load(&s->errors)?1:0);
    }
    if(pid<0) return 1;
    ludp_ctx *ctx=ludp_attach(pid);
    if(!ctx) { perror("attach"); failed=1; goto cleanup; }
    {
        uintptr_t tiny=(uintptr_t)dlsym(RTLD_DEFAULT,"tiny_target");
        unsigned char before[16], after[16];
        u64 rejected=0;
        if(ludp_mem_read(ctx,tiny,before,sizeof before) ||
           ludp_probe_add_addr_ex(ctx,"tiny_target",tiny,flags,&rejected)==0 ||
           ludp_mem_read(ctx,tiny,after,sizeof after) || memcmp(before,after,sizeof before)) {
            fputs("FAIL unsafe short function accepted/modified\n",stderr); failed=1; goto detach;
        }
    }
    if (fallback) ctx->fsgsbase = false;
    for(int i=0;i<9;i++) {
        int rc = i == 8 ? ludp_probe_add_addr_ex(ctx, names[i], (uintptr_t)dlsym(RTLD_DEFAULT,names[i]), flags, &ids[i]) :
                          ludp_probe_add_ex(ctx,"libtest.so",names[i],flags,&ids[i]);
        if(rc) {
            fprintf(stderr,"FAIL insertion %s\n",names[i]); failed=1; goto detach;
        }
    }
    if(flags & LUDP_PROBE_RETURN) {
        u64 rejected=0;
        if(ludp_probe_add_ex(ctx,"libtest.so","six",flags,&rejected)==0) {
            fputs("FAIL duplicate return probe accepted\n",stderr); failed=1; goto detach;
        }
    }
    atomic_store(&s->go,1);
    ludp_resume(ctx);
    double deadline=now()+10;
    while(!atomic_load(&s->done) && now()<deadline) { ludp_poll(ctx); usleep(100); }
    if(!atomic_load(&s->done) || atomic_load(&s->errors)) { fputs("FAIL target execution\n",stderr); failed=1; goto detach; }
    for(;;) {
        ludp_event ev[128]; int n=0;
        if(ludp_events_read(ctx,ev,128,&n)) { failed=1; break; }
        if(!n) break;
        for(int j=0;j<n;j++) {
            ludp_event *e=&ev[j]; int k;
            for(k=0;k<9;k++) if(ids[k]==e->probe_id) break;
            if(k==9 || (e->type!=LUDP_EVENT_ENTRY && e->type!=LUDP_EVENT_RETURN)) { failed=1; continue; }
            int ret=e->type==LUDP_EVENT_RETURN;
            counts[k][ret]++;
            if(!e->ret_addr) failed=1;
            if(k==0 && (e->args[0]!=1 || e->args[1]!=2 || e->args[2]!=3 || e->args[3]!=4 || e->args[4]!=5 || e->args[5]!=0xfedcba9876543210ULL)) failed=1;
            if(flags & LUDP_PROBE_RETURN) {
                if(e->call_id==0 || e->call_id>=2048) { failed=1; continue; }
                if(!ret) entries[e->call_id]=*e;
                else if(entries[e->call_id].probe_id!=e->probe_id ||
                        entries[e->call_id].ret_addr!=e->ret_addr ||
                        memcmp(entries[e->call_id].args,e->args,sizeof e->args)) failed=1;
            }
            if(!ret) continue;
            switch(k) {
            case 0: if(e->retval!=1ULL+6+15+28+55+0xfedcba9876543210ULL*13) failed=1; break;
            case 1: if((int64_t)e->retval!=-130) failed=1; break;
            case 2: if(e->retval!=(uintptr_t)s) failed=1; break;
            case 3: { double x; memcpy(&x,e->xmm0,8); if(x!=4.0) failed=1; break; }
            case 4: if(e->retval!=0xdeadbeef12345678ULL || e->retval_hi!=0xdeadbeef12345679ULL) failed=1; break;
            case 5: if(e->retval!=891) failed=1; break;
            case 6: if(e->retval!=1+e->args[0]*(e->args[0]+1)/2) failed=1; break;
            case 7: if(e->retval!=e->args[0]+42) failed=1; break;
            case 8: if(e->retval!=0xfedcba9876543210ULL) failed=1; break;
            }
        }
    }
    for(int i=0;i<9;i++) {
        u64 expected=i==0?400:i==6?9:i==7?2:1;
        if(counts[i][0]!=expected || counts[i][1]!=((flags&LUDP_PROBE_RETURN)?expected:0)) {
            fprintf(stderr,"FAIL %s entry=%lu return=%lu expected=%lu\n",names[i],counts[i][0],counts[i][1],expected); failed=1;
        }
    }
    if(ludp_overflow(ctx) || ludp_return_missed(ctx)) failed=1;
detach:
    ludp_detach(ctx);
cleanup:
    if(failed) kill(pid,SIGKILL); else atomic_store(&s->release,1);
    waitpid(pid,&status,0);
    if(!WIFEXITED(status)||WEXITSTATUS(status)) failed=1;
    munmap(s,sizeof *s);
    printf("F1-F3 flags=%u %s\n",flags,failed?"FAIL":"PASS");
    return failed;
}
static int ring_full(void)
{
    int failed=0,status=0; u64 id=0;
    s=mmap(NULL,sizeof *s,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(s==MAP_FAILED) return 1;
    pid_t pid=fork();
    if(pid==0) {
        while(!atomic_load(&s->go)) __asm__ volatile("pause");
        for(u64 i=0;i<1000;i++) if(six(i,2,3,4,5,6)!=i+182) atomic_fetch_add(&s->errors,1);
        atomic_store(&s->done,1);
        while(atomic_load(&s->go)!=2) __asm__ volatile("pause");
        if(six(1000,2,3,4,5,6)!=1182) atomic_fetch_add(&s->errors,1);
        atomic_store(&s->done,2);
        while(!atomic_load(&s->release)) __asm__ volatile("pause");
        _exit(0);
    }
    if(pid<0) return 1;
    ludp_ctx *ctx=ludp_attach(pid);
    if(!ctx) { failed=1; goto cleanup; }
    /* Test-only capacity reduction; the allocated mapping remains unchanged. */
    ctx->ring_cap=16; ctx->ring_mask=15;
    if(ludp_mem_write(ctx,ctx->ring_addr+RO_CAP,&ctx->ring_cap,8) ||
       ludp_mem_write(ctx,ctx->ring_addr+RO_MASK,&ctx->ring_mask,8) ||
       ludp_probe_add_ex(ctx,"libtest.so","six",3,&id)) { failed=1; goto cleanup; }
    atomic_store(&s->go,1); ludp_resume(ctx);
    double deadline=now()+10;
    while(atomic_load(&s->done)!=1 && now()<deadline) { ludp_poll(ctx); usleep(100); }
    if(atomic_load(&s->done)!=1) { failed=1; goto cleanup; }
    ludp_event ev[32]; int n=0;
    if(ludp_events_read(ctx,ev,32,&n) || n!=16 || ludp_overflow(ctx)!=1984 || ludp_return_missed(ctx)) failed=1;
    for(int i=0;i<n;i++) {
        if(ev[i].probe_id!=id || ev[i].args[0]!=(u64)i/2 || ev[i].args[5]!=6 ||
           ev[i].type!=(i%2?LUDP_EVENT_RETURN:LUDP_EVENT_ENTRY) ||
           (i%2 && ev[i].retval!=ev[i].args[0]+182)) failed=1;
    }
    atomic_store(&s->go,2);
    while(atomic_load(&s->done)!=2 && now()<deadline) { ludp_poll(ctx); usleep(100); }
    if(atomic_load(&s->done)!=2 || ludp_events_read(ctx,ev,32,&n) || n!=2 ||
       ev[1].retval!=1182 || ev[1].call_id!=1001 || atomic_load(&s->errors)) failed=1;
cleanup:
    if(ctx) ludp_detach(ctx);
    if(failed) kill(pid,SIGKILL); else atomic_store(&s->release,1);
    waitpid(pid,&status,0);
    if(!WIFEXITED(status)||WEXITSTATUS(status)) failed=1;
    munmap(s,sizeof *s);
    printf("ring overflow/recovery %s\n",failed?"FAIL":"PASS");
    return failed;
}
int main(void) { return run(LUDP_PROBE_ENTRY,0) | run(LUDP_PROBE_ENTRY|LUDP_PROBE_RETURN,0) |
                       run(LUDP_PROBE_ENTRY|LUDP_PROBE_RETURN,1) | ring_full(); }
#endif
