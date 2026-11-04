#ifndef HOST_SHA1_STREAM_H
#define HOST_SHA1_STREAM_H
/* Original incremental SHA-1 implementation of FIPS 180-4, for disc identities.
 * Bounded memory; never concatenate or map a whole disc image. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef struct { uint32_t h[5]; uint64_t bytes; uint8_t block[64]; } host_sha1_t;
static inline uint32_t host_sha1_rol(uint32_t x, unsigned n) { return x<<n | x>>(32-n); }
static inline void host_sha1_init(host_sha1_t *s) {
    static const uint32_t initial[5]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    memcpy(s->h,initial,sizeof(initial)); s->bytes=0;
}
static inline void host_sha1_block(host_sha1_t *s) {
    uint32_t w[80],a=s->h[0],b=s->h[1],c=s->h[2],d=s->h[3],e=s->h[4];
    unsigned i;
    for(i=0;i<16;++i) { const uint8_t *p=s->block+i*4; w[i]=(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
    for(;i<80;++i) w[i]=host_sha1_rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    for(i=0;i<80;++i) {
        uint32_t f,k,t;
        if(i<20) { f=(b&c)|(~b&d); k=0x5a827999; }
        else if(i<40) { f=b^c^d; k=0x6ed9eba1; }
        else if(i<60) { f=(b&c)|(b&d)|(c&d); k=0x8f1bbcdc; }
        else { f=b^c^d; k=0xca62c1d6; }
        t=host_sha1_rol(a,5)+f+e+k+w[i]; e=d; d=c; c=host_sha1_rol(b,30); b=a; a=t;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}
static inline void host_sha1_update(host_sha1_t *s, const void *data, size_t size) {
    const uint8_t *p=data;
    while(size) {
        size_t at=(size_t)(s->bytes%64), n=64-at;
        if(n>size) n=size;
        memcpy(s->block+at,p,n); p+=n; size-=n; s->bytes+=n;
        if(s->bytes%64==0) host_sha1_block(s);
    }
}
static inline void host_sha1_final(host_sha1_t *s, uint8_t out[20]) {
    uint64_t bits=s->bytes*8; uint8_t padding[72]={0x80}, length[8];
    size_t n=64-(size_t)(s->bytes%64);
    if(n<9) n+=64;
    for(unsigned i=0;i<8;++i) length[7-i]=(uint8_t)(bits>>(i*8));
    host_sha1_update(s,padding,n-8); host_sha1_update(s,length,8);
    for(unsigned i=0;i<20;++i) out[i]=(uint8_t)(s->h[i/4]>>(24-8*(i%4)));
}
#endif
