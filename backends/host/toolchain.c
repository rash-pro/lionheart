#define _POSIX_C_SOURCE 200809L
#include "toolchain.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#define mkdir_private(p) _mkdir(p)
#define TC_NULL "NUL"
#else
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <curl/curl.h>
#define mkdir_private(p) mkdir(p,0700)
#define TC_NULL "/dev/null"
#endif
#define PATH_CAP 4096
#define BLOCK 65536

typedef struct {
    const host_tc_manifest *m;
    host_tc_progress progress;
    void *user;
    host_tc_error *error;
    int cancelled;
    uint64_t done;
} install;
static int fail(install *i, const char *why) {
    if(i->error) snprintf(i->error->reason,sizeof(i->error->reason),"%s",why);
    return 0;
}
static int tick(install *i, host_tc_phase phase, uint64_t done, uint64_t total) {
    if(i->progress && i->progress(i->user,phase,done,total)) {
        i->cancelled=1; return fail(i,"Cancelled");
    }
    return 1;
}
static int path(char *out, const char *a, const char *b) {
    int n=snprintf(out,PATH_CAP,"%s/%s",a,b);
    return n>0 && n<PATH_CAP;
}
static int directory(const char *name) {
    char copy[PATH_CAP]; struct stat st;
    if(strlen(name)>=sizeof(copy)) return 0;
    strcpy(copy,name);
    for(char *p=copy+1;;++p) if(*p=='/' || !*p) {
#ifdef _WIN32
        if(p==copy+2 && copy[1]==':' && *p=='/') continue;
#endif
        char save=*p; *p=0;
        if((mkdir_private(copy) && errno!=EEXIST) || stat(copy,&st) || !S_ISDIR(st.st_mode)) return 0;
        *p=save; if(!save) break;
    }
    return 1;
}
static int parent(const char *name) {
    char copy[PATH_CAP], *slash;
    if(strlen(name)>=sizeof(copy)) return 0;
    strcpy(copy,name); slash=strrchr(copy,'/');
    if(!slash) return 0;
    *slash=0; return directory(copy);
}
/* Never traverse links left by an interrupted or externally modified stage. */
static int remove_tree(const char *name) {
#ifdef _WIN32
    DWORD attr=GetFileAttributesA(name);
    if(attr==INVALID_FILE_ATTRIBUTES) return 1;
    if(attr&FILE_ATTRIBUTE_REPARSE_POINT)
        return (attr&FILE_ATTRIBUTE_DIRECTORY)?RemoveDirectoryA(name):DeleteFileA(name);
    if(attr&FILE_ATTRIBUTE_DIRECTORY) {
        char pattern[PATH_CAP], child[PATH_CAP]; WIN32_FIND_DATAA d;
        if(!path(pattern,name,"*")) return 0;
        HANDLE h=FindFirstFileA(pattern,&d);
        if(h!=INVALID_HANDLE_VALUE) {
            do { if(!strcmp(d.cFileName,".") || !strcmp(d.cFileName,"..")) continue;
                if(!path(child,name,d.cFileName) || !remove_tree(child)) { FindClose(h); return 0; }
            } while(FindNextFileA(h,&d)); FindClose(h);
        }
        return RemoveDirectoryA(name)!=0;
    }
    return DeleteFileA(name)!=0;
#else
    struct stat st;
    if(lstat(name,&st)) return errno==ENOENT;
    if(S_ISDIR(st.st_mode)) {
        DIR *d=opendir(name); struct dirent *e; int ok=1; char child[PATH_CAP];
        if(!d) return 0;
        while((e=readdir(d))) {
            if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
            if(!path(child,name,e->d_name) || !remove_tree(child)) { ok=0; break; }
        }
        closedir(d); return ok && !rmdir(name);
    }
    return !unlink(name);
#endif
}
static int safe_name(const char *s) {
    const char *start=s;
    if(!*s || *s=='/') return 0;
    for(;;++s) {
        if(*s=='\\' || *s==':' || (unsigned char)*s<32) { if(*s) return 0; }
        if(*s=='/' || !*s) {
            size_t n=(size_t)(s-start);
            if(n && (s[-1]=='.' || s[-1]==' ')) return 0;
            if((n==1 && *start=='.') || (n==2 && !memcmp(start,"..",2)) || (!n && *s)) return 0;
            if(!*s) return 1;
            start=s+1;
        }
    }
}
static int digest(const char *name, char hex[65], uint64_t *size, install *i) {
    FILE *f=fopen(name,"rb"); unsigned char b[BLOCK]; size_t n; lhg_sha256 h;
    if(!f) return 0;
    *size=0; lhg_sha256_init(&h);
    while((n=fread(b,1,sizeof(b),f))) {
        lhg_sha256_add(&h,b,n); *size+=n;
        if(i && !tick(i,HOST_TC_VERIFY,*size,i->m->size)) { fclose(f); return 0; }
    }
    int ok=!ferror(f); fclose(f); lhg_sha256_finish(&h,hex); return ok;
}
static int regular(const char *name) {
#ifdef _WIN32
    DWORD a=GetFileAttributesA(name);
    return a!=INVALID_FILE_ATTRIBUTES && !(a&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
#else
    struct stat st; return !lstat(name,&st) && S_ISREG(st.st_mode);
#endif
}
static int installed(const char *dest, const host_tc_manifest *m) {
    char name[PATH_CAP], line[256], expected[256], hex[65]; uint64_t size;
    if(!path(name,dest,"receipt") || !regular(name)) return 0;
    FILE *f=fopen(name,"rb"); if(!f) return 0;
    snprintf(expected,sizeof(expected),"toolchain-v1 %s %s self-check=passed\n",m->version,m->sha256);
    int ok=fgets(line,sizeof(line),f) && !strcmp(line,expected);
    for(size_t j=0;ok && j<m->file_count;++j) {
        ok=safe_name(m->files[j]) && path(name,dest,m->files[j]) && regular(name) && digest(name,hex,&size,NULL);
        if(ok) { snprintf(expected,sizeof(expected),"%s %llu\n",hex,(unsigned long long)size);
            ok=fgets(line,sizeof(line),f) && !strcmp(line,expected); }
    }
    if(ok) ok=fgetc(f)==EOF;
    fclose(f); return ok;
}
static int locations(const char *data,const host_tc_manifest *m,char *root,char *dest) {
    return safe_name(m->version) && !strchr(m->version,'/') && path(root,data,"toolchain") && path(dest,root,m->version);
}
host_tc_state host_tc_query(const char *data,const host_tc_manifest *m) {
    char root[PATH_CAP],dest[PATH_CAP],work[PATH_CAP]; struct stat st;
    if(!locations(data,m,root,dest)) return HOST_TC_PARTIAL;
    if(installed(dest,m)) return HOST_TC_INSTALLED;
    if(!stat(dest,&st)) return HOST_TC_PARTIAL;
    if(!path(work,root,"download.part") || !stat(work,&st)) return HOST_TC_PARTIAL;
    if(!path(work,root,"stage") || !stat(work,&st)) return HOST_TC_PARTIAL;
    return HOST_TC_ABSENT;
}
int host_tc_executable(const char *data,const host_tc_manifest *m,char *out,size_t cap) {
    char root[PATH_CAP],dest[PATH_CAP],exe[PATH_CAP];
    if(!locations(data,m,root,dest) || !installed(dest,m) || !path(exe,dest,m->executable) || strlen(exe)>=cap) return 0;
    strcpy(out,exe); return 1;
}
static unsigned u16(const unsigned char *p) { return p[0]|((unsigned)p[1]<<8); }
static uint32_t u32(const unsigned char *p) { return u16(p)|((uint32_t)u16(p+2)<<16); }
static int read_at(FILE *f,uint64_t at,void *b,size_t n) {
    return at<=0x7fffffff && !fseek(f,(long)at,SEEK_SET) && fread(b,1,n,f)==n;
}
/* The central directory supplies sizes for ZIPs using data descriptors. Every
 * entry is validated, including entries outside the retained subset. */
static int extract(install *i,const char *archive,const char *stage) {
    FILE *f=fopen(archive,"rb"); unsigned char end[65557],h[46],local[30];
    unsigned char *seen=calloc(i->m->file_count,1); int ok=0;
    uint64_t tail=i->m->size<sizeof(end)?i->m->size:sizeof(end), base=i->m->size-tail;
    if(!f || !seen || !read_at(f,base,end,(size_t)tail)) goto done;
    long e=(long)tail-22;
    for(;e>=0;--e) if(u32(end+e)==0x06054b50 && e+22+(long)u16(end+e+20)==(long)tail) break;
    if(e<0 || u16(end+e+4) || u16(end+e+6) || u16(end+e+8)!=u16(end+e+10)) goto done;
    uint64_t cd=u32(end+e+16), cd_end=cd+u32(end+e+12);
    if(cd_end!=base+(uint64_t)e) goto done;
    unsigned count=u16(end+e+10);
    for(unsigned k=0;k<count;++k) {
        char name[PATH_CAP],out[PATH_CAP];
        if(!tick(i,HOST_TC_EXTRACT,k,count) || cd+46>cd_end || !read_at(f,cd,h,46) || u32(h)!=0x02014b50) goto done;
        unsigned len=u16(h+28),extra=u16(h+30),comment=u16(h+32),method=u16(h+10),mode=u32(h+38)>>16;
        uint64_t packed=u32(h+20),unpacked=u32(h+24),offset=u32(h+42);
        if(!len || len>=sizeof(name) || cd+46+len+extra+comment>cd_end || !read_at(f,cd+46,name,len)) goto done;
        name[len]=0; cd+=46+len+extra+comment;
        if(strlen(name)!=len || !safe_name(name) || (u16(h+8)&~(8|2048)) || u16(h+34) ||
           (method!=0 && method!=8) || ((mode&0170000) && (mode&0170000)!=0100000 && (mode&0170000)!=0040000) ||
           (u32(h+38)&0x400)) goto done;
        size_t j; const char *rel=NULL; size_t prefix=strlen(i->m->prefix);
        if(!strncmp(name,i->m->prefix,prefix)) rel=name+prefix;
        for(j=0;j<i->m->file_count;++j) if(rel && !strcmp(rel,i->m->files[j])) break;
        if(j==i->m->file_count) continue;
        if(seen[j] || unpacked>512*1024*1024 || !path(out,stage,rel) || !parent(out)) goto done;
        seen[j]=1;
        if(!read_at(f,offset,local,30) || u32(local)!=0x04034b50 || u16(local+6)!=u16(h+8) || u16(local+8)!=method || u16(local+26)!=len) goto done;
        char lname[PATH_CAP];
        if(!read_at(f,offset+30,lname,len) || memcmp(name,lname,len)) goto done;
        uint64_t pos=offset+30+len+u16(local+28),left=packed,written=0;
        if(pos+packed>u32(end+e+16)) goto done;
        FILE *o=fopen(out,"wb"); if(!o) goto done;
        unsigned char in[BLOCK],buf[BLOCK]; z_stream z={0};
        int good=method==0 || inflateInit2(&z,-MAX_WBITS)==Z_OK,ended=0;
        uLong crc=crc32(0,NULL,0);
        while(good && left) {
            size_t n=left>BLOCK?BLOCK:(size_t)left;
            if(!read_at(f,pos,in,n)) {good=0;break;}
            pos+=n;left-=n;z.next_in=in;z.avail_in=(uInt)n;
            do {
                size_t got=n;
                if(method==8) {
                    z.next_out=buf; z.avail_out=BLOCK;
                    int r=inflate(&z,Z_NO_FLUSH);
                    if(r!=Z_OK && r!=Z_STREAM_END) {good=0;break;}
                    got=BLOCK-z.avail_out;
                    if(r==Z_STREAM_END) {ended=1;if(left || z.avail_in) good=0;}
                }
                const unsigned char *bytes=method==0?in:buf;
                written+=got;
                if(written>unpacked || fwrite(bytes,1,got,o)!=got || !tick(i,HOST_TC_EXTRACT,written,unpacked)) good=0;
                crc=crc32(crc,bytes,(uInt)got);
                if(method==0 || ended) break;
            } while(good && (z.avail_in || !z.avail_out));
        }
        if(method==8) inflateEnd(&z);
        if(fclose(o)) good=0;
        if(!good || written!=unpacked || crc!=u32(h+16) || (method==8 && !ended)) goto done;
#ifndef _WIN32
        if(!strcmp(rel,i->m->executable) && chmod(out,0700)) goto done;
#endif
    }
    if(cd!=cd_end) goto done;
    ok=1; for(size_t j=0;j<i->m->file_count;++j) if(!seen[j]) ok=0;
done:
    if(f) fclose(f);
    free(seen); return ok || (i->cancelled?0:fail(i,"Invalid ZIP or missing retained file"));
}
static int run(const char *exe,const char *const *args) {
#ifdef _WIN32
    char command[32768];size_t at=0;
    for(size_t a=0;args[a];++a) {
        if(at+4>=sizeof(command)) return 0;
        command[at++]='"';
        for(const char *p=args[a];*p;) {
            size_t slashes=0;while(*p=='\\') {++slashes;++p;}
            size_t copies=slashes*((!*p || *p=='"')?2:1);
            if(at+copies+4>=sizeof(command)) return 0;
            while(copies--) command[at++]='\\';
            if(*p=='"') command[at++]='\\';
            if(*p) command[at++]=*p++;
        }
        command[at++]='"';command[at++]=' ';
    }
    command[at]=0;wchar_t wide[32768],application[PATH_CAP];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,command,-1,wide,32768) ||
       !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,exe,-1,application,PATH_CAP)) return 0;
    STARTUPINFOW startup={0};PROCESS_INFORMATION process={0};startup.cb=sizeof(startup);
    if(!CreateProcessW(application,wide,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process)) return 0;
    WaitForSingleObject(process.hProcess,INFINITE);DWORD status=1;
    GetExitCodeProcess(process.hProcess,&status);CloseHandle(process.hProcess);CloseHandle(process.hThread);
    return status==0;
#else
    pid_t pid=fork(); if(pid<0) return 0;
    if(!pid) { int fd=open(TC_NULL,O_WRONLY); if(fd>=0) {dup2(fd,1);dup2(fd,2);close(fd);} execv(exe,(char *const *)args); _exit(127); }
    int status; while(waitpid(pid,&status,0)<0) if(errno!=EINTR) return 0;
    return WIFEXITED(status) && !WEXITSTATUS(status);
#endif
}
static int self_check(install *i,const char *stage) {
    char exe[PATH_CAP],source[PATH_CAP],object[PATH_CAP],library[PATH_CAP];
    if(!path(exe,stage,i->m->executable) || !path(source,stage,"check.c") || !path(object,stage,"check.o") || !path(library,stage,"check.dll")) return 0;
    const char *version[]={exe,"cc","--version",NULL};
    if(!tick(i,HOST_TC_CHECK,0,3) || !run(exe,version)) return fail(i,"Compiler version self-check failed");
    FILE *f=fopen(source,"wb"); if(!f) return fail(i,"Cannot write compiler probe");
    const char *code="__attribute__((visibility(\"default\"))) int toolchain_probe(void) { return 42; }\n";
    int ok=fputs(code,f)>=0; if(fclose(f)) ok=0;
    const char *compile[]={exe,"cc","-mcpu=baseline","-O2","-std=c99","-fPIC","-c",source,"-o",object,NULL};
    const char *link[]={exe,"cc","-shared","-nostdlib","-fuse-ld=lld",object,"-o",library,NULL};
    ok=ok && tick(i,HOST_TC_CHECK,1,3) && run(exe,compile) && regular(object) &&
        tick(i,HOST_TC_CHECK,2,3) && run(exe,link) && regular(library);
    remove(source);remove(object);remove(library);
    return ok || fail(i,"Compiler compile/link self-check failed");
}
#ifndef _WIN32
static size_t received(char *buf,size_t size,size_t count,void *user) {
    struct {install *i;FILE *f;} *s=user; size_t n=size*count;
    if(n>s->i->m->size-s->i->done) return 0;
    size_t wrote=fwrite(buf,1,n,s->f);s->i->done+=wrote;
    if(!tick(s->i,HOST_TC_DOWNLOAD,s->i->done,s->i->m->size)) return 0;
    return wrote;
}
static int transfer_progress(void *user,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d) {
    install *i=user;(void)a;(void)b;(void)c;(void)d;
    return !tick(i,HOST_TC_DOWNLOAD,i->done,i->m->size);
}
#endif
static int download(install *i,const char *url,const char *target) {
    struct stat st; uint64_t offset=!stat(target,&st)?(uint64_t)st.st_size:0;
    if(offset>i->m->size) {remove(target);offset=0;}
    if(offset==i->m->size) return 1;
    FILE *f=fopen(target,offset?"ab":"wb"); if(!f) return fail(i,"Cannot write download");
    i->done=offset; int ok=0;
#ifdef _WIN32
    wchar_t wide[8192],host[1024],resource[8192]; URL_COMPONENTS parts={0};
    HINTERNET session=NULL,connection=NULL,request=NULL;
    parts.dwStructSize=sizeof(parts);parts.lpszHostName=host;parts.dwHostNameLength=1024;
    parts.lpszUrlPath=resource;parts.dwUrlPathLength=8192;
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,url,-1,wide,8192) || !WinHttpCrackUrl(wide,0,0,&parts)) goto net_done;
    session=WinHttpOpen(L"Lionheart",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,NULL,NULL,0);
    if(!session) goto net_done;
    WinHttpSetTimeouts(session,10000,10000,10000,1000);
    connection=WinHttpConnect(session,host,parts.nPort,0);if(!connection) goto net_done;
    request=WinHttpOpenRequest(connection,L"GET",resource,NULL,NULL,NULL,parts.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
    if(!request) goto net_done;
    wchar_t range[128];if(offset) {swprintf(range,128,L"Range: bytes=%llu-\r\n",(unsigned long long)offset);WinHttpAddRequestHeaders(request,range,(DWORD)-1,WINHTTP_ADDREQ_FLAG_ADD);}
    if(!WinHttpSendRequest(request,NULL,0,NULL,0,0,0) || !WinHttpReceiveResponse(request,NULL)) goto net_done;
    DWORD status=0,len=sizeof(status);
    if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,NULL,&status,&len,NULL) || status!=(offset?206:200)) goto net_done;
    unsigned char buffer[BLOCK];DWORD got,available;
    while(tick(i,HOST_TC_DOWNLOAD,i->done,i->m->size)) {
        if(!WinHttpQueryDataAvailable(request,&available)) break;
        if(!available) {ok=1;break;}
        DWORD want=available>sizeof(buffer)?sizeof(buffer):available;
        if(!WinHttpReadData(request,buffer,want,&got) || !got) break;
        if(got>i->m->size-i->done || fwrite(buffer,1,got,f)!=got) break;
        i->done+=got;
    }
net_done:
    if(request) WinHttpCloseHandle(request);
    if(connection) WinHttpCloseHandle(connection);
    if(session) WinHttpCloseHandle(session);
#else
    CURL *c=curl_easy_init();
    if(c) {
        struct {install *i;FILE *f;} sink={i,f};
        curl_easy_setopt(c,CURLOPT_URL,url);
        curl_easy_setopt(c,CURLOPT_PROTOCOLS_STR,"http,https");
        curl_easy_setopt(c,CURLOPT_REDIR_PROTOCOLS_STR,"http,https");
        curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L);
        curl_easy_setopt(c,CURLOPT_FAILONERROR,1L);
        curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,10L);
        curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,1L);
        curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,30L);
        curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,received);
        curl_easy_setopt(c,CURLOPT_WRITEDATA,&sink);
        curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,transfer_progress);
        curl_easy_setopt(c,CURLOPT_XFERINFODATA,i);
        curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);
        if(offset) curl_easy_setopt(c,CURLOPT_RESUME_FROM_LARGE,(curl_off_t)offset);
        CURLcode result=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
        ok=result==CURLE_OK && status==(offset?206:200);
        curl_easy_cleanup(c);
    }
#endif
    if(i->done!=i->m->size) ok=0;
    if(fclose(f)) ok=0;
    return ok || fail(i,i->cancelled?"Cancelled":"Download failed; retry to resume");
}
int host_tc_install(const char *data,const host_tc_manifest *m,const char *local,
                    host_tc_progress progress,void *user,host_tc_error *error) {
    install i={m,progress,user,error,0,0};
    char root[PATH_CAP],dest[PATH_CAP],stage[PATH_CAP],part[PATH_CAP],lock[PATH_CAP],receipt[PATH_CAP];
    int ok=0; if(error) error->reason[0]=0;
    if(!locations(data,m,root,dest) || !path(stage,root,"stage") || !path(part,root,"download.part") || !path(lock,root,"install.lock") || !directory(root)) return fail(&i,"Cannot create toolchain directory");
    if(strcmp(m->format,"zip")) return fail(&i,"Unsupported archive format");
    for(size_t j=0;j<m->file_count;++j) if(!safe_name(m->files[j])) return fail(&i,"Invalid retained path");
#ifdef _WIN32
    HANDLE guard=CreateFileA(lock,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(guard==INVALID_HANDLE_VALUE) return fail(&i,"Another installer is active; retry");
#else
    int guard=open(lock,O_CREAT|O_RDWR,0600);
    if(guard<0) return fail(&i,"Cannot open install lock");
    if(flock(guard,LOCK_EX|LOCK_NB)) {close(guard);return fail(&i,"Another installer is active; retry");}
    fcntl(guard,F_SETFD,FD_CLOEXEC);
#endif
    if(installed(dest,m)) {ok=1;goto done;}
    if(!remove_tree(stage) || !remove_tree(dest)) {fail(&i,"Cannot clear incomplete install");goto done;}
    const char *archive=local?local:part;
    if(!local) {
        if(!m->url_count) {fail(&i,"No download URL configured for this host");goto done;}
        for(size_t u=0;u<m->url_count;++u) { if(download(&i,m->urls[u],part)) {ok=1;break;} if(i.cancelled) break; }
        if(!ok) goto done;
        ok=0;
    }
    char hex[65];uint64_t size;
    if(!digest(archive,hex,&size,&i)) {if(!i.cancelled) fail(&i,"Cannot read archive");goto done;}
    if(size!=m->size || strcmp(hex,m->sha256)) {
        if(!local) remove(part);
        fail(&i,size!=m->size?"Archive size mismatch":"Archive SHA-256 mismatch");goto done;
    }
    if(!directory(stage) || !extract(&i,archive,stage) || !self_check(&i,stage) || !tick(&i,HOST_TC_COMMIT,0,1)) goto done;
    if(!path(receipt,stage,"receipt")) goto done;
    FILE *f=fopen(receipt,"wb"); if(!f) {fail(&i,"Cannot write receipt");goto done;}
    ok=fprintf(f,"toolchain-v1 %s %s self-check=passed\n",m->version,m->sha256)>0;
    for(size_t j=0;ok && j<m->file_count;++j) {
        char name[PATH_CAP];ok=path(name,stage,m->files[j]) && digest(name,hex,&size,NULL);
        if(ok) ok=fprintf(f,"%s %llu\n",hex,(unsigned long long)size)>0;
    }
    if(fflush(f)) ok=0;
#ifndef _WIN32
    if(fsync(fileno(f))) ok=0;
#endif
    if(fclose(f)) ok=0;
    if(ok) ok=!rename(stage,dest);
    if(!ok) fail(&i,"Cannot publish toolchain receipt");
    if(ok) remove(part);
done:
    if(!ok) remove_tree(stage);
#ifdef _WIN32
    CloseHandle(guard);
#else
    close(guard);
#endif
    return ok;
}
