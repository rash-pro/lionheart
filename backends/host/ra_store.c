#define _POSIX_C_SOURCE 200809L
#include "ra_store.h"
#include "ra_http.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

char *host_ra_file_read(const char *path,size_t *size,size_t limit,int private_file) {
    int fd; struct stat st; char *data=NULL;
#ifdef _WIN32
    fd=open(path,O_RDONLY|O_BINARY);
#else
    fd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
#endif
    if (fd<0) return NULL;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<0 || (uint64_t)st.st_size>limit) goto done;
#ifndef _WIN32
    if (private_file && (st.st_uid!=geteuid() || (st.st_mode&0777)!=0600)) goto done;
#else
    (void)private_file;
#endif
    size_t at=0,n=(size_t)st.st_size;
    data=malloc(n+1);
    if (!data) goto done;
    while (at<n) {
        int got=(int)read(fd,data+at,(unsigned)(n-at));
        if (got<=0) { host_ra_erase(data,n); free(data); data=NULL; goto done; }
        at+=(size_t)got;
    }
    data[n]=0; *size=n;
done:
    close(fd); return data;
}
int host_ra_file_write(const char *path,const void *data,size_t size) {
    size_t length=strlen(path)+16,at=0;
    char *temp=malloc(length); int fd=-1,ok=0;
    if (!temp) return 0;
    snprintf(temp,length,"%s.XXXXXX",path);
#ifdef _WIN32
    if (_mktemp_s(temp,length)) goto done;
    fd=open(temp,O_WRONLY|O_CREAT|O_EXCL|O_BINARY,0600);
#else
    fd=mkstemp(temp);
#endif
    if (fd<0) goto done;
    while (at<size) {
        int wrote=(int)write(fd,(const char *)data+at,(unsigned)(size-at));
        if (wrote<=0) goto done;
        at+=(size_t)wrote;
    }
#ifdef _WIN32
    if (_commit(fd)) goto done;
#else
    if (fchmod(fd,0600) || fsync(fd)) goto done;
#endif
    if (close(fd)) { fd=-1; goto done; }
    fd=-1;
#ifdef _WIN32
    ok=MoveFileExA(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    ok=rename(temp,path)==0;
    if (ok) {
        char *slash=strrchr(temp,'/');
        if (slash) { *slash=0; int dir=open(temp,O_RDONLY|O_DIRECTORY|O_CLOEXEC); if (dir>=0) { ok=fsync(dir)==0; close(dir); } else ok=0; *slash='/'; }
    }
#endif
done:
    if (fd>=0) close(fd);
    if (!ok) unlink(temp);
    free(temp); return ok;
}
static int field(const char *s,size_t cap) {
    size_t n=strlen(s);
    if (!n || n>=cap) return 0;
    for (size_t i=0;i<n;++i) if ((unsigned char)s[i]<32 || (unsigned char)s[i]==127) return 0;
    return 1;
}
int host_ra_credentials_write(const char *path,const char *user,const char *token) {
    char data[512]; int n,ok;
    if (!field(user,128) || !field(token,256)) return 0;
    n=snprintf(data,sizeof(data),"ra-user=%s\nra-token=%s\n",user,token);
    ok=host_ra_file_write(path,data,(size_t)n); host_ra_erase(data,sizeof(data)); return ok;
}
int host_ra_credentials_read(const char *path,char *user,size_t uc,char *token,size_t tc) {
    size_t n=0; char *data=host_ra_file_read(path,&n,511,1); int ok=0;
    if (!data) return 0;
    char *line=strchr(data,'\n'), *end=line ? strchr(line+1,'\n') : NULL;
    if (line && end && end==data+n-1 && !strncmp(data,"ra-user=",8) && !strncmp(line+1,"ra-token=",9)) {
        *line=0; *end=0;
        if (field(data+8,uc) && field(line+10,tc)) { strcpy(user,data+8); strcpy(token,line+10); ok=1; }
    }
    host_ra_erase(data,n); free(data); return ok;
}
