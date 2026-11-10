/* Stored entries make archive bytes independent of compression-library versions. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct { char *name; uint32_t size, crc, offset, mode; } entry;
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
        unsigned char buffer[65536]; size_t n; FILE *in; entry *e;
        line[strcspn(line,"\r\n")]=0;
        if(!line[0] || count==1024 || strlen(line)>UINT16_MAX) fail("invalid list");
        e=&entries[count++]; e->name=malloc(strlen(line)+1);
        if(!e->name) fail("allocation failed");
        strcpy(e->name,line); e->size=0; e->crc=crc32(0,0,0);
        e->mode=strcmp(line,argv[3])==0?0100755:0100644;
        in=fopen(line,"rb"); if(!in) fail(line);
        while((n=fread(buffer,1,sizeof(buffer),in))) {
            if(n>UINT32_MAX-e->size) fail("file too large");
            e->size+=(uint32_t)n; e->crc=crc32(e->crc,buffer,(uInt)n);
        }
        if(ferror(in) || fseek(in,0,SEEK_SET)) fail("read failed");
        e->offset=position();
        put(0x04034b50,4); put(20,2); put(0,2); put(0,2);
        put(0,2); put(33,2); put(e->crc,4); put(e->size,4); put(e->size,4);
        put((uint32_t)strlen(line),2); put(0,2); name(e);
        while((n=fread(buffer,1,sizeof(buffer),in)))
            if(fwrite(buffer,1,n,out)!=n) fail("write failed");
        if(ferror(in) || fclose(in)) fail("read failed");
    }
    if(ferror(list) || fclose(list)) fail("list read failed");
    uint32_t start=position();
    for(unsigned i=0;i<count;++i) {
        entry *e=&entries[i];
        put(0x02014b50,4); put(0x0314,2); put(20,2); put(0,2); put(0,2);
        put(0,2); put(33,2); put(e->crc,4); put(e->size,4); put(e->size,4);
        put((uint32_t)strlen(e->name),2); put(0,2); put(0,2); put(0,2); put(0,2);
        put(e->mode<<16,4); put(e->offset,4); name(e); free(e->name);
    }
    uint32_t length=position()-start;
    put(0x06054b50,4); put(0,2); put(0,2); put(count,2); put(count,2);
    put(length,4); put(start,4); put(0,2);
    if(fclose(out)) fail("close failed");
    return 0;
}
