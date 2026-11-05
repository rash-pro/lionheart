/* Linux diagnostic linker probe. Guest code and scheduling are unchanged. */
#define _GNU_SOURCE
#include "hw.h"
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <ucontext.h>

static cpu_t *cpu;
#ifdef SUBSYSTEMS
static struct { uint64_t vb; uintptr_t host; uint32_t guest; } samples[200000];
static volatile sig_atomic_t sampled;
static timer_t sample_timer;
static int timer_active;
static void sample(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    if (cpu && cpu->hw && sampled < 200000) {
        unsigned n=sampled;
        samples[n].vb=cpu->hw->vblanks;
        samples[n].host=((ucontext_t *)context)->uc_mcontext.gregs[REG_RIP];
        samples[n].guest=cpu->pc;
        sampled=n+1;
    }
}
#endif

static FILE *out;
static double wall_start, cpu_start;
static uint64_t cycles, interpreted, compiled, idle, gte, cd;
static double costs[6], stamp;
static int phase;
static uint64_t calls[6];
static double now(clockid_t clock) {
    struct timespec t; clock_gettime(clock, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec * 1e-6;
}
static int enter(int n) {
    double t = now(CLOCK_THREAD_CPUTIME_ID); int old = phase;
    costs[phase] += t - stamp; stamp = t; phase = n; ++calls[n]; return old;
}
static void leave(int old) {
    double t = now(CLOCK_THREAD_CPUTIME_ID);
    costs[phase] += t - stamp; stamp = t; phase = old;
}
void __real_cpu_init(cpu_t *c);
#ifdef SUBSYSTEMS
static void save_samples(void) {
    if (timer_active) timer_delete(sample_timer);
    char path[4096]; snprintf(path,sizeof(path),"%s.samples",getenv("FF8_FRAME_TIMINGS"));
    FILE *f=fopen(path,"w"); if (!f) return;
    fprintf(f,"# cpu_init=%lx\nvblank,host,guest\n",(unsigned long)(uintptr_t)__real_cpu_init);
    for (int i=0;i<sampled;i++) fprintf(f,"%llu,%lx,%08x\n",(unsigned long long)samples[i].vb,(unsigned long)samples[i].host,samples[i].guest);
    fclose(f);
}
#endif
void __wrap_cpu_init(cpu_t *c) {
    __real_cpu_init(c); cpu = c;
    if (out) fclose(out);
#ifdef SUBSYSTEMS
    if (timer_active) timer_delete(sample_timer);
    timer_active=0;
    sampled=0;
#endif
    const char *path = getenv("FF8_FRAME_TIMINGS");
    if (!path) exit(90);
    out = fopen(path, "w"); if (!out) exit(91);
    fputs("vblank,wall_ms,cpu_ms,cycles,interpreted,compiled,idle,gte,cd_bytes,guest_ms,gte_ms,gpu_ms,cd_dma_ms,spu_ms,present_ms,gte_calls,gpu_calls,cd_dma_calls,spu_calls,pc\n", out);
    cycles=interpreted=compiled=idle=gte=cd=0;
    memset(costs,0,sizeof(costs)); memset(calls,0,sizeof(calls)); phase=0;
    wall_start = now(CLOCK_MONOTONIC); cpu_start = stamp = now(CLOCK_THREAD_CPUTIME_ID);
#ifdef SUBSYSTEMS
    struct sigaction sa={0}; sa.sa_sigaction=sample; sa.sa_flags=SA_SIGINFO; sigaction(SIGPROF,&sa,NULL);
    struct sigevent event={0};
    event.sigev_notify=SIGEV_THREAD_ID; event.sigev_signo=SIGPROF;
    event._sigev_un._tid=(pid_t)syscall(SYS_gettid);
    if (timer_create(CLOCK_THREAD_CPUTIME_ID,&event,&sample_timer)) exit(92);
    struct itimerspec timer={{0,1000000},{0,1000000}};
    if (timer_settime(sample_timer,0,&timer,NULL)) exit(93);
    timer_active=1;
    static int registered;
    if (!registered) { atexit(save_samples); registered=1; }
#endif
}
void __real_hw_destroy(cpu_t *c);
void __wrap_hw_destroy(cpu_t *c) {
    if (c == cpu) {
#ifdef SUBSYSTEMS
        if (timer_active) timer_delete(sample_timer);
        timer_active=0;
#endif
        cpu=NULL;
    }
    __real_hw_destroy(c);
}
void __real_host_present(gpu_t *g);
void __wrap_host_present(gpu_t *g) {
    int old = enter(5); __real_host_present(g); leave(old);
    double wall = now(CLOCK_MONOTONIC), time = now(CLOCK_THREAD_CPUTIME_ID);
    costs[phase] += time - stamp;
    fprintf(out, "%llu,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%08x\n",
        (unsigned long long)cpu->hw->vblanks, wall-wall_start, time-cpu_start,
        (unsigned long long)(cpu->cycles-cycles),
        (unsigned long long)(cpu->interp_instructions-interpreted),
        (unsigned long long)(cpu->recomp_instructions-compiled),
        (unsigned long long)(cpu->idle_instructions-idle),
        (unsigned long long)(cpu->gte_commands-gte),
        (unsigned long long)(cpu->hw->cd.bytes_dma-cd),
        costs[0],costs[1],costs[2],costs[3],costs[4],costs[5],
        (unsigned long long)calls[1],(unsigned long long)calls[2],
        (unsigned long long)calls[3],(unsigned long long)calls[4],cpu->pc);
    cycles=cpu->cycles; interpreted=cpu->interp_instructions; compiled=cpu->recomp_instructions;
    idle=cpu->idle_instructions; gte=cpu->gte_commands; cd=cpu->hw->cd.bytes_dma;
    memset(costs,0,sizeof(costs)); memset(calls,0,sizeof(calls));
    wall_start=now(CLOCK_MONOTONIC); cpu_start=stamp=now(CLOCK_THREAD_CPUTIME_ID);
}
#ifdef SUBSYSTEMS
#define WRAP_VOID(name,group,params,args) void __real_##name params; void __wrap_##name params { int old=enter(group); __real_##name args; leave(old); }
#define WRAP_RET(type,name,group,params,args) type __real_##name params; type __wrap_##name params { int old=enter(group); type result=__real_##name args; leave(old); return result; }
WRAP_VOID(gte_cmd,1,(cpu_t *c,uint32_t v),(c,v))
WRAP_RET(uint32_t,gte_read,1,(cpu_t *c,unsigned r,int control),(c,r,control))
WRAP_VOID(gte_write,1,(cpu_t *c,unsigned r,uint32_t v,int control),(c,r,v,control))
WRAP_VOID(gpu_gp0,2,(gpu_t *g,uint32_t v),(g,v))
WRAP_VOID(gpu_gp0_words,2,(gpu_t *g,const uint8_t *b,unsigned n),(g,b,n))
WRAP_VOID(gpu_gp1,2,(gpu_t *g,uint32_t v),(g,v))
WRAP_RET(uint32_t,gpu_read,2,(gpu_t *g),(g))
WRAP_VOID(cdrom_event,3,(cpu_t *c),(c))
WRAP_VOID(dma_event,3,(cpu_t *c),(c))
WRAP_VOID(dma_sync,3,(cpu_t *c),(c))
WRAP_VOID(spu_advance,4,(spu_t *s,uint64_t v),(s,v))
WRAP_VOID(spu_write,4,(spu_t *s,unsigned a,uint16_t v),(s,a,v))
WRAP_RET(uint16_t,spu_read,4,(spu_t *s,unsigned a),(s,a))
WRAP_VOID(spu_dma_write,4,(spu_t *s,uint32_t v),(s,v))
WRAP_RET(uint32_t,spu_dma_read,4,(spu_t *s),(s))
#endif
