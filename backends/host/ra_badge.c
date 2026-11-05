#include "ra_store.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static int paeth(int a,int b,int c) {
    int p=a+b-c,pa=abs(p-a),pb=abs(p-b),pc=abs(p-c);
    return pa<=pb && pa<=pc ? a : pb<=pc ? b : c;
}
/* Badge PNGs need no animation or metadata. Bounds and CRCs are checked before
 * inflation; unsupported formats leave the ordinary placeholder visible. */
uint8_t *host_ra_badge_decode(const void *bytes,size_t length) {
    const uint8_t *p=bytes;
    uint8_t palette[256][4],*compressed=NULL,*raw=NULL,*out=NULL;
    unsigned width=0,height=0,depth=0,type=0,channels=0,colours=0;
    size_t used=0,at=8; int ended=0,header=0,transparent=-1;
    uint8_t transparent_rgb[3]={0}; int have_rgb=0;
    if (length<45 || length>1024*1024 || memcmp(p,"\x89PNG\r\n\x1a\n",8)) return NULL;
    memset(palette,255,sizeof(palette));
    compressed=malloc(length);
    if (!compressed) return NULL;
    while (at+12<=length) {
        uint32_t n=be32(p+at); const uint8_t *kind=p+at+4,*data=p+at+8;
        if (n>length-at-12 || crc32(0,kind,n+4)!=be32(data+n)) goto done;
        if (!memcmp(kind,"IHDR",4)) {
            if (header || at!=8 || n!=13) goto done;
            header=1; width=be32(data); height=be32(data+4); depth=data[8]; type=data[9];
            if (!width || !height || width>512 || height>512 || data[10] || data[11] || data[12]) goto done;
            channels=type==6 ? 4 : type==2 ? 3 : type==4 ? 2 : (type==0 || type==3) ? 1 : 0;
            if (!channels || (depth!=8 && !((type==0 || type==3) && (depth==1 || depth==2 || depth==4)))) goto done;
        } else if (!header) goto done;
        else if (!memcmp(kind,"PLTE",4)) {
            if (!n || n>768 || n%3) goto done;
            colours=n/3;
            for (unsigned i=0;i<colours;++i) memcpy(palette[i],data+i*3,3);
        } else if (!memcmp(kind,"tRNS",4)) {
            if (type==3) { if (n>colours) goto done; for (unsigned i=0;i<n;++i) palette[i][3]=data[i]; }
            else if (type==0 && n==2) transparent=data[0]*256+data[1];
            else if (type==2 && n==6) { for (unsigned i=0;i<3;++i) transparent_rgb[i]=data[i*2+1]; have_rgb=1; }
            else goto done;
        } else if (!memcmp(kind,"IDAT",4)) { memcpy(compressed+used,data,n); used+=n; }
        else if (!memcmp(kind,"IEND",4)) { if (n) goto done; ended=1; break; }
        else if (!(kind[0]&32)) goto done;
        at+=n+12;
    }
    if (!ended || !used || (type==3 && !colours)) goto done;
    size_t stride=((size_t)width*channels*depth+7)/8,bpp=(channels*depth+7)/8;
    uLongf size=(uLongf)((stride+1)*height);
    raw=malloc(size);
    if (!raw || uncompress(raw,&size,compressed,(uLong)used)!=Z_OK || size!=(stride+1)*height) goto done;
    for (unsigned y=0;y<height;++y) {
        uint8_t *row=raw+y*(stride+1)+1,*prev=y ? row-stride-1 : NULL;
        unsigned filter=row[-1]; if (filter>4) goto done;
        for (size_t x=0;x<stride;++x) {
            int a=x>=bpp ? row[x-bpp] : 0,b=prev ? prev[x] : 0,c=prev && x>=bpp ? prev[x-bpp] : 0;
            row[x]=(uint8_t)(row[x]+(filter==1 ? a : filter==2 ? b : filter==3 ? (a+b)/2 : filter==4 ? paeth(a,b,c) : 0));
        }
    }
    out=malloc(64*64*4);
    if (!out) goto done;
    for (unsigned y=0;y<64;++y) for (unsigned x=0;x<64;++x) {
        unsigned sx=x*width/64,sy=y*height/64;
        const uint8_t *row=raw+sy*(stride+1)+1,*s=row+sx*channels;
        uint8_t *d=out+(y*64+x)*4;
        unsigned sample=depth==8 ? row[sx] : (row[sx*depth/8]>>(8-depth-(sx*depth%8)))&((1u<<depth)-1);
        d[3]=255;
        if (type==3) {
            if (sample>=colours) { free(out); out=NULL; goto done; }
            memcpy(d,palette[sample],4);
        } else if (type==0 || type==4) {
            unsigned v=type==4 ? s[0] : sample*255/((1u<<depth)-1);
            d[0]=d[1]=d[2]=(uint8_t)v;
            d[3]=type==4 ? s[1] : (int)sample==transparent ? 0 : 255;
        } else { memcpy(d,s,channels); if (have_rgb && !memcmp(d,transparent_rgb,3)) d[3]=0; }
    }
done:
    free(raw); free(compressed); return out;
}
