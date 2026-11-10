#define _POSIX_C_SOURCE 200809L
#include "toolchain.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
static const char *mode;
static int progress(void *u,host_tc_phase p,uint64_t n,uint64_t total) {
    (void)total;
    if(!strcmp(mode,"hold") && p==HOST_TC_EXTRACT && n==0) {
        char marker[4096];snprintf(marker,sizeof(marker),"%s/holding",(const char *)u);
        FILE *f=fopen(marker,"wb");if(f) fclose(f);
#ifdef _WIN32
        Sleep(1000);
#else
        struct timespec delay={1,0};nanosleep(&delay,NULL);
#endif
    }
    if(!strcmp(mode,"kill") && p==HOST_TC_COMMIT) _exit(77);
    return (!strcmp(mode,"cancel-download") && p==HOST_TC_DOWNLOAD && n>0) ||
           (!strcmp(mode,"cancel-extract") && p==HOST_TC_EXTRACT && n>0);
}
int main(int argc,char **argv) {
    if(argc>1 && !strcmp(argv[1],"cc")) {
        for(int a=2;a+1<argc;++a) if(!strcmp(argv[a],"-o")) {
            FILE *out=fopen(argv[a+1],"wb");if(!out) return 1;fputs("probe",out);fclose(out);
        }
        return 0;
    }
    if(argc==4 && !strcmp(argv[3],"pinned")) {
        host_tc_manifest m=*host_tc_pinned();host_tc_error e;
        const char *archive=strcmp(argv[2],"download")?argv[2]:NULL;
        const char *url=getenv("LIONHEART_TOOLCHAIN_TEST_URL");
        if(url) {m.urls=&url;m.url_count=1;}
        int ok=host_tc_install(argv[1],&m,archive,NULL,NULL,&e);
        printf("state=%d result=%d reason=%s\n",host_tc_query(argv[1],&m),ok,e.reason);
        return ok?0:1;
    }
    if(argc<5) return 2;
    #ifdef _WIN32
    const char *files[]={"zig.exe"};
#else
    const char *files[]={"zig"};
#endif
    const char *urls[2]={argv[4],argc>5?argv[5]:argv[4]};
    char hex[65];uint64_t size=0;lhg_sha256 h;lhg_sha256_init(&h);
    FILE *f=fopen(argv[2],"rb"); if(!f) return 2;
    unsigned char b[65536];size_t n;
    while((n=fread(b,1,sizeof(b),f))) {lhg_sha256_add(&h,b,n);size+=n;}
    fclose(f);lhg_sha256_finish(&h,hex);
    mode=argv[3];
    if(!strcmp(mode,"checksum")) hex[0]=hex[0]=='0'?'1':'0';
    if(!strcmp(mode,"size")) size++;
    host_tc_manifest m={"test","zip","payload/",files[0],hex,size,urls,argc>5?2:1,files,1};
    if(!strcmp(mode,"no-url")) {m.urls=NULL;m.url_count=0;}
    host_tc_error error;
    printf("before=%d\n",host_tc_query(argv[1],&m));
    int ok=host_tc_install(argv[1],&m,!strcmp(argv[4],"offline")?argv[2]:NULL,progress,argv[1],&error);
    printf("after=%d result=%d reason=%s\n",host_tc_query(argv[1],&m),ok,error.reason);
    return ok?0:1;
}
