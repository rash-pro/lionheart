/* Private disc-derived streams stay under build/. Each record is two LE words. */
#include "gpu.h"
#include "gpu_stream.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GP0, GP1, VBLANK, READ, STATUS, SNAPSHOT, ORIGIN, PROJECTION, EPOCH, AMBIGUOUS };
static const char signature[8]={'F','F','8','G','P','U','R',
#ifdef FF8_PGXP
'2'
#else
'1'
#endif
};

#ifdef FF8_PGXP
#include "pgxp.h"
#include "cpu.h"
#endif
/* This TU replaces the archive's GPU object in record builds, exposing the
 * pointer-free parser state without adding a checkpoint API to the runtime. */
#include "../../runtime/gpu.c"
static FILE *stream;
#ifdef FF8_PGXP
static FILE *context_stream;
static void close_context(void) { if (context_stream && fclose(context_stream)) abort(); }
#endif
static unsigned frames,from,captured,configured;
static uint64_t records;
static void configure(void) {
    if (!configured) {
        const char *v=getenv("FF8_GPU_FROM"); from=v?(unsigned)strtoul(v,NULL,10):0;
        configured=1;
    }
}
static void close_stream(void) { if (stream && fclose(stream)) abort(); }
static void record(unsigned kind,uint32_t value) {
    uint8_t words[8];
    gpu_stream_put32(words,kind); gpu_stream_put32(words+4,value);
    configure();
    if (from && !captured) return;
    if (!stream) {
        const char *path=getenv("FF8_GPU_STREAM");
        if (!path) return;
        stream=fopen(path,"wb");
        if (!stream || setvbuf(stream,NULL,_IOFBF,1024*1024) ||
            fwrite(signature,1,sizeof(signature),stream)!=sizeof(signature)) abort();
        atexit(close_stream);
    }
    if (fwrite(words,sizeof(words),1,stream)!=1) abort();
    ++records;
}
#ifdef FF8_PGXP
static void projection(void *context,const struct cpu *c,uint32_t key,int64_t x,int64_t y,uint32_t z) {
    uint8_t payload[20];
    gpu_stream_put32(payload,(uint32_t)x); gpu_stream_put32(payload+4,(uint32_t)((uint64_t)x>>32));
    gpu_stream_put32(payload+8,(uint32_t)y); gpu_stream_put32(payload+12,(uint32_t)((uint64_t)y>>32));
    gpu_stream_put32(payload+16,z);
    (void)context; (void)c;
    configure();
    if (from && !captured) return;
    record(PROJECTION,key);
    if (!stream || fwrite(payload,sizeof(payload),1,stream)!=1) abort();
    if (c) {
        const char *path=getenv("FF8_PGXP_CONTEXT");
        if (path) {
            uint32_t matrix=2166136261u;
            if (!context_stream) {
                context_stream=fopen(path,"w"); if (!context_stream) abort();
                fprintf(context_stream,"frame,pc,ra,key,x_q16,y_q16,z,matrix,trx,try,trz\n");
                atexit(close_context);
            }
            for (unsigned i=0;i<8;++i) matrix=(matrix^c->gte_ctrl[i])*16777619u;
            fprintf(context_stream,"%u,%u,%u,%u,%lld,%lld,%u,%u,%d,%d,%d\n",frames,c->pc,c->r[31],key,
                    (long long)x,(long long)y,z,matrix,(int32_t)c->gte_ctrl[5],(int32_t)c->gte_ctrl[6],(int32_t)c->gte_ctrl[7]);
        }
    }
}
static void ambiguity(void *context,uint32_t key) { (void)context; record(AMBIGUOUS,key); }
void __real_pgxp_bind(struct gpu *,pgxp_mode_t);
void __wrap_pgxp_bind(struct gpu *g,pgxp_mode_t mode) {
    __real_pgxp_bind(g,mode);
    configure();
    if (getenv("FF8_GPU_STREAM")) pgxp_record_sink(projection,NULL);
}
#endif
void __real_gpu_gp0(gpu_t *,uint32_t);
void __real_gpu_gp0_words(gpu_t *,const uint8_t *,unsigned);
void __real_gpu_gp1(gpu_t *,uint32_t);
void __real_gpu_vblank(gpu_t *);
uint32_t __real_gpu_read(gpu_t *);
uint32_t __real_gpu_status(gpu_t *);
void __wrap_gpu_gp0(gpu_t *g,uint32_t word) { record(GP0,word); __real_gpu_gp0(g,word); }
void __wrap_gpu_gp1(gpu_t *g,uint32_t word) { record(GP1,word); __real_gpu_gp1(g,word); }
void __wrap_gpu_vblank(gpu_t *g) { ++frames; record(VBLANK,0); __real_gpu_vblank(g); }
void __wrap_gpu_gp0_words(gpu_t *g,const uint8_t *p,unsigned count) {
    unsigned i;
    for (i=0;i<count;++i) {
        const uint8_t *v=p+i*4;
        record(GP0,v[0]|((uint32_t)v[1]<<8)|((uint32_t)v[2]<<16)|((uint32_t)v[3]<<24));
    }
    __real_gpu_gp0_words(g,p,count);
}
uint32_t __wrap_gpu_read(gpu_t *g) { uint32_t v=__real_gpu_read(g); record(READ,v); return v; }
uint32_t __wrap_gpu_status(gpu_t *g) {
    uint32_t v=__real_gpu_status(g);
    configure();
    if (from && !captured && frames>=from && (v&(1u<<26))) {
        captured=1; record(SNAPSHOT,sizeof(*g));
        if (!stream || fwrite(g,sizeof(*g),1,stream)!=1) abort();
        record(ORIGIN,frames);
#ifdef FF8_PGXP
        /* Seed every live generation; the snapshot itself contains no pointers. */
        for (unsigned age=PGXP_GENERATIONS; age-- > 0;) {
            pgxp_visit(age,projection,ambiguity,NULL);
            if (age) record(EPOCH,0);
        }
#endif
    }
    record(STATUS,v); return v;
}
