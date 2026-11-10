
#include "sha1_stream.h"
#include <stdio.h>
int main(void) {
    host_sha1_t s; unsigned char data[37], out[20]; size_t n;
    host_sha1_init(&s);
    while ((n=fread(data,1,sizeof(data),stdin))) host_sha1_update(&s,data,n);
    host_sha1_final(&s,out);
    for (unsigned i=0;i<20;++i) printf("%02x",out[i]);
    return ferror(stdin) ? 1 : 0;
}
