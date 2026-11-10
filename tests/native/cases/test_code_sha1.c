#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv){
    FILE *f;long size;unsigned prefix,i;uint8_t *data,digest[20];
    if(argc!=3)return 2;
    prefix=(unsigned)strtoul(argv[2],NULL,10);
    f=fopen(argv[1],"rb");if(!f)return 2;
    if(fseek(f,0,SEEK_END)||(size=ftell(f))<0||fseek(f,0,SEEK_SET))return 2;
    data=malloc((size_t)size+prefix+1);if(!data)return 2;
    if(fread(data+prefix,1,(size_t)size,f)!=(size_t)size)return 2;
    fclose(f);
    code_sha1(size==0&&prefix==0?NULL:data+prefix,(size_t)size,digest);
    for(i=0;i<20;++i)printf("%02x",digest[i]);
    putchar('\n');free(data);return 0;
}
