#define _POSIX_C_SOURCE 200809L
#include "options.h"
#include "gui.h"
#include <SDL.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int parents(const char *path) {
    char *copy=strdup(path), *p;
    if (!copy) return 0;
    for (p=copy+1; *p; ++p) if (*p=='/' || *p=='\\') {
        char c=*p; *p=0;
        if (mkdir(copy,0700) && errno!=EEXIST) { free(copy); return 0; }
        *p=c;
    }
    free(copy); return 1;
}
/* Bounded reads preserve arbitrary unknown lines, including their line endings. */
static char *read_line(FILE *f) {
    size_t n=0, cap=256; int c;
    char *s=malloc(cap);
    if (!s) return NULL;
    while ((c=fgetc(f))!=EOF) {
        if (n+2>cap) {
            char *next;
            if (cap>=1024*1024) { free(s); errno=EFBIG; return NULL; }
            cap*=2; next=realloc(s,cap);
            if (!next) { free(s); return NULL; } s=next;
        }
        s[n++]=(char)c; if (c=='\n') break;
    }
    if (!n) { free(s); return NULL; }
    s[n]=0; return s;
}
static char *value_start(char *line,const char *key) {
    char *p=line; size_t n=strlen(key);
    while (isspace((unsigned char)*p)) ++p;
    if (strncmp(p,key,n)) return NULL;
    p+=n; while (*p==' ' || *p=='\t') ++p;
    if (*p++!='=') return NULL;
    while (*p==' ' || *p=='\t') ++p;
    return p;
}
static char *suffix(char *p,const char *key) {
    char *s=p,*end=p+strlen(p);
    int path_value=0;
    for (size_t i=0;i<host_option_count;++i) if (!strcmp(key,host_options[i].key))
        path_value=host_options[i].type==HOST_VALUE_MEDIA || host_options[i].type==HOST_VALUE_PATH;
    for (;*s;++s) if ((*s=='#' || *s==';') && (s==p || isspace((unsigned char)s[-1]))) {
        if (path_value && s[1] && !isspace((unsigned char)s[1])) continue;
        end=s; break;
    }
    while (end>p && isspace((unsigned char)end[-1])) --end;
    return end;
}
int host_options_write(host_options_t *o,const char *key,const char *value,char *error,size_t size) {
    static const char header[]="# Lionheart settings";
    FILE *in=NULL,*out=NULL; char tmp[4352]={0}, *line; int fd=-1,found=0,has_header=0,ok=0,last='\n';
    struct stat st; unsigned attempt; int n;
    if (!host_options_set(o,key,value)) { snprintf(error,size,host_gui_string("ini.fail.invalid"),key); return 0; }
    if (!*o->config_path) { snprintf(error,size,"%s",host_gui_string("ini.fail.no_path")); return 0; }
    if (!parents(o->config_path)) goto failure;
    in=fopen(o->config_path,"rb");
    if (!in && errno!=ENOENT) goto failure;
    for (attempt=0;attempt<32;++attempt) {
        n=snprintf(tmp,sizeof(tmp),"%s.tmp-%llu-%u",o->config_path,(unsigned long long)SDL_GetPerformanceCounter(),attempt);
        if (n<0 || (size_t)n>=sizeof(tmp)) { errno=ENAMETOOLONG; goto failure; }
        fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);
        if (fd>=0 || errno!=EEXIST) break;
    }
    if (fd<0) goto failure;
#ifndef _WIN32
    if (in && !fstat(fileno(in),&st) && fchmod(fd,st.st_mode&0777)) goto failure;
#else
    (void)st;
#endif
    out=fdopen(fd,"wb"); if (!out) goto failure; fd=-1;
    while (in && (line=read_line(in))) {
        char *p=value_start(line,key);
        const char *comment=line;
        while (*comment==' ' || *comment=='\t') ++comment;
        if (!strncmp(comment,header,sizeof(header)-1) &&
            strspn(comment+sizeof(header)-1," \t\r\n")==strlen(comment+sizeof(header)-1)) has_header=1;
        if (p) {
            char *tail=suffix(p,key);
            if (fwrite(line,1,(size_t)(p-line),out)!=(size_t)(p-line) || fputs(value,out)==EOF || fputs(tail,out)==EOF) { free(line); goto failure; }
            found=1;
        } else if (fputs(line,out)==EOF) { free(line); goto failure; }
        last=(unsigned char)line[strlen(line)-1]; free(line);
    }
    if (in && (ferror(in) || !feof(in))) goto failure;
    if (!found && ((last!='\n' && fputc('\n',out)==EOF) ||
        (!has_header && fprintf(out,"%s\n",header)<0) || fprintf(out,"%s=%s\n",key,value)<0)) goto failure;
    if (fflush(out)) goto failure;
#ifdef _WIN32
    if (_commit(fileno(out))) goto failure;
#else
    if (fsync(fileno(out))) goto failure;
#endif
    if (fclose(out)) { out=NULL; goto failure; } out=NULL;
    if (in) { fclose(in); in=NULL; }
    if (rename(tmp,o->config_path)) goto failure;
    ok=1;
failure:
    if (!ok) snprintf(error,size,host_gui_string("ini.fail.detail"),o->config_path,strerror(errno));
    if (in) fclose(in);
    if (out) fclose(out);
    if (fd>=0) close(fd);
    if (!ok && *tmp) remove(tmp);
    return ok;
}
