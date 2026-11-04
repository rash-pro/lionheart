#define FF8_PLAY
#define FF8_SPU_CACHE_TEST
#include "../runtime/spu.c"
#include <assert.h>
#include <stdio.h>
static uint32_t random_value(uint32_t *seed){*seed=*seed*1664525u+1013904223u;return *seed;}
int main(void) {
    spu_t *s=spu_create();uint32_t seed=2319;unsigned predictor,shift,iter,n;
    assert(s);
    for(predictor=0;predictor<16;++predictor)for(shift=0;shift<16;++shift)for(iter=0;iter<12;++iter) {
        uint8_t data[16];int16_t history[2],expected[28],prefix[3];voice_t *v;unsigned i;
        n=iter%24;v=&s->voices[n];v->address=0x2000;
        if(iter%3!=1){
            for(i=0;i<16;++i)s->ram[0x2000+i]=(uint8_t)random_value(&seed);
            s->ram[0x2000]=(uint8_t)(predictor*16+shift);
            s->ram[0x2001]=(uint8_t)(iter&7);
        }
        for(i=0;i<2;++i)v->history[i]=(int16_t)random_value(&seed);
        /* The same full key must hit even when voice samples were replaced. */
        for(i=0;i<3;++i) {
            unsigned j;unsigned before=s->decode_hits;uint64_t blocks=s->blocks;
            memcpy(data,s->ram+0x2000,16);memcpy(history,v->history,4);memcpy(prefix,v->samples+28,6);
            decode_block(data,history,expected);
            voice_decode(s,n);
            assert(!memcmp(v->samples,prefix,6)&&!memcmp(v->samples+3,expected,56));
            assert(!memcmp(v->history,history,4)&&v->flags==data[1]&&v->decoded);
            assert(s->blocks==blocks+1);
            if(i==1)assert(s->decode_hits==before+1);
            if(i==0){
                /* Restore the exact input key, but poison previous output. */
                memcpy(v->history,s->decode_memo[n].history,4);
                for(j=3;j<31;++j)v->samples[j]=(int16_t)random_value(&seed);
            }
            if(i==1)s->ram[0x2002]^=0x80; /* data mutation forces a miss */
        }
    }
    assert(s->decode_hits);printf("PASS decoder cache independent block oracle hits=%u; headers, predictors, shifts, RAM/history mutations and copied voice outputs\n",s->decode_hits);
    spu_destroy(s);return 0;
}
