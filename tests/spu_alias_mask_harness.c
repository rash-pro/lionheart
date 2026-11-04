#define FF8_PLAY
#include "../runtime/spu.c"
static int old_voices_alias(const spu_t *s, unsigned count, uint32_t voices, unsigned capture, uint32_t base) {
    unsigned n;
    if (REG(s, 0x1ac) & 12) {
        capture = 1;
        if ((REG(s, 0x1aa) & 0x80) && (uint32_t)REG(s, 0x1a2) * 8 < base)
            base = (uint32_t)REG(s, 0x1a2) * 8;
    }
    if (!capture && base == 0x80000) return 0;
    for (n = 0; n < 24; ++n) {
        const voice_t *v = &s->voices[n];
        if (!(voices & (1u << n))) continue;
        unsigned delay = v->delay, decoded = v->decoded, pos = v->position, frac = v->fraction;
        unsigned step = REG(s, n * 16 + 4), left = count, f = v->flags;
        uint32_t addr = v->address, repeat = (uint32_t)REG(s, n * 16 + 14) * 8 & ~15u;
        if (s->key_on & (1u << n)) { delay = 2; decoded = pos = frac = f = 0; addr = (uint32_t)REG(s, n * 16 + 6) * 8 & ~15u; }
        if (n && (flags(s, 0x190) & (1u << n))) step = 0x4000;
        if (step > 0x4000) step = 0x4000;
        if (!(s->key_on & (1u << n)) && decoded && !step) continue;
        if (decoded && (f & 3) == 3 && repeat == addr && s->ram[(addr + 1) & RAM_MASK] == f) {
            if ((capture && addr < 0x1000) || addr + 16 > base) return 1;
            continue;
        }
        if (delay >= left) continue;
        left -= delay;
        while (left) {
            unsigned span = left, fraction;
            if (!decoded) {
                if ((capture && addr < 0x1000) || addr + 16 > base) return 1;
                f = s->ram[(addr + 1) & RAM_MASK]; decoded = 1;
                if (f & 4) repeat = addr;
            }
            if (step) {
                unsigned distance = (28 - pos) * 4096 - frac;
                if (step * span > distance) span = (distance + step - 1) / step;
            }
            fraction = frac + step * span; pos += fraction >> 12; frac = fraction & 0xfff; left -= span;
            if (pos >= 28) { pos -= 28; decoded = 0; addr = f & 1 ? repeat : (addr + 16) & RAM_MASK; }
        }
    }
    return 0;
}
#include <assert.h>
#include <stdio.h>
static uint32_t rv(uint32_t *s){*s=*s*1664525u+1013904223u;return *s;}
int main(void){
    spu_t *s=spu_create();uint32_t seed=1924;unsigned i,n;uint64_t cases=0;
    assert(s);for(i=0;i<sizeof(s->ram);++i)s->ram[i]=(uint8_t)(rv(&seed)>>24);
    for(i=0;i<1200;++i){
        unsigned count=rv(&seed)%4097,capture=rv(&seed)&1;uint32_t base=(rv(&seed)&1)?0x80000:(rv(&seed)&0x7fff0),mask=rv(&seed)&0xffffff,want=0;
        REG(s,0x1ac)=rv(&seed)&12;REG(s,0x1aa)=rv(&seed);REG(s,0x1a2)=rv(&seed);
        REG(s,0x190)=rv(&seed);REG(s,0x192)=rv(&seed);s->key_on=rv(&seed)&0xffffff;
        for(n=0;n<24;++n){voice_t *v=&s->voices[n];v->address=rv(&seed)&0x7fff0;v->position=rv(&seed)%28;v->fraction=rv(&seed)&4095;v->delay=rv(&seed)%4;v->decoded=rv(&seed)&1;v->flags=rv(&seed)&7;REG(s,n*16+4)=rv(&seed);REG(s,n*16+6)=rv(&seed);REG(s,n*16+14)=rv(&seed);}
        for(n=0;n<24;++n)if((mask&(1u<<n))&&old_voices_alias(s,count,1u<<n,capture,base))want|=1u<<n;
        assert(voices_alias_mask(s,count,mask,capture,base)==want);
        assert(!!voices_alias(s,count,mask,capture,base)==!!old_voices_alias(s,count,mask,capture,base));++cases;
    }
    printf("PASS alias-mask equivalence %llu full-state random capture/reverb/PMON/pitch/key proofs\n",(unsigned long long)cases);spu_destroy(s);return 0;
}
