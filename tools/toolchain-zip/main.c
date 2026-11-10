/* Writes the release archives of the trimmed compiler and of the Windows player.
 * Everything that does not depend on the compressor is fixed (order, dates, modes),
 * so two builds with the same zlib give the same bytes; a third of the download is
 * worth that condition. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct { char *name; uint32_t size, packed, crc, offset, mode; } entry;
static FILE *out;
static void fail(const char *why) { fprintf(stderr, "toolchain-zip: %s\n", why); exit(1); }
static void put(uint32_t v, unsigned n) {
    while(n--) { if(fputc(v & 255, out)==EOF) fail("write failed"); v >>= 8; }
}
static uint32_t position(void) {
    long n=ftell(out);
    if(n<0 || (unsigned long)n>UINT32_MAX) fail("ZIP32 limit exceeded");
    return (uint32_t)n;
}
static void name(const entry *e) {
    size_t n=strlen(e->name);
    if(fwrite(e->name,1,n,out)!=n) fail("write failed");
}
int main(int argc, char **argv) {
    FILE *list; entry entries[1024]; unsigned count=0; char line[4096];
    if(argc!=4) fail("expected output.zip file-list executable-entry");
    list=fopen(argv[2],"rb"); out=fopen(argv[1],"wb");
    if(!list || !out) fail("cannot open output or list");
    while(fgets(line,sizeof(line),list)) {
        unsigned char *data, *packed; long length; FILE *in; entry *e; z_stream z;
        line[strcspn(line,"\r\n")]=0;
        if(!line[0] || count==1024 || strlen(line)>UINT16_MAX) fail("invalid list");
        e=&entries[count++]; e->name=malloc(strlen(line)+1);
        if(!e->name) fail("allocation failed");
        strcpy(e->name,line);
        e->mode=strcmp(line,argv[3])==0?0100755:0100644;
        in=fopen(line,"rb"); if(!in) fail(line);
        if(fseek(in,0,SEEK_END) || (length=ftell(in))<0 || fseek(in,0,SEEK_SET)) fail("read failed");
        if((unsigned long)length>UINT32_MAX/2) fail("file too large");
        e->size=(uint32_t)length; data=malloc(e->size?e->size:1);
        if(!data || fread(data,1,e->size,in)!=e->size || fclose(in)) fail("read failed");
        e->crc=crc32(crc32(0,0,0),data,e->size);
        memset(&z,0,sizeof(z));
        if(deflateInit2(&z,9,Z_DEFLATED,-MAX_WBITS,9,Z_DEFAULT_STRATEGY)!=Z_OK) fail("deflate failed");
        uLong bound=deflateBound(&z,e->size); packed=malloc(bound?bound:1);
        if(!packed || bound>UINT32_MAX) fail("allocation failed");
        z.next_in=data; z.avail_in=e->size; z.next_out=packed; z.avail_out=(uInt)bound;
        if(deflate(&z,Z_FINISH)!=Z_STREAM_END) fail("deflate failed");
        e->packed=(uint32_t)z.total_out; deflateEnd(&z);
        e->offset=position();
        put(0x04034b50,4); put(20,2); put(0,2); put(8,2);
        put(0,2); put(33,2); put(e->crc,4); put(e->packed,4); put(e->size,4);
        put((uint32_t)strlen(line),2); put(0,2); name(e);
        if(fwrite(packed,1,e->packed,out)!=e->packed) fail("write failed");
        free(packed); free(data);
    }
    if(ferror(list) || fclose(list)) fail("list read failed");
    uint32_t start=position();
    for(unsigned i=0;i<count;++i) {
        entry *e=&entries[i];
        put(0x02014b50,4); put(0x0314,2); put(20,2); put(0,2); put(8,2);
        put(0,2); put(33,2); put(e->crc,4); put(e->packed,4); put(e->size,4);
        put((uint32_t)strlen(e->name),2); put(0,2); put(0,2); put(0,2); put(0,2);
        put(e->mode<<16,4); put(e->offset,4); name(e); free(e->name);
    }
    uint32_t length=position()-start;
    put(0x06054b50,4); put(0,2); put(0,2); put(count,2); put(count,2);
    put(length,4); put(start,4); put(0,2);
    if(fclose(out)) fail("close failed");
    return 0;
}
