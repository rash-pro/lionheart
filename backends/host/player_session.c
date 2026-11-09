#define _POSIX_C_SOURCE 200809L
#include "player_paths.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#define pipe(fds) _pipe(fds,65536,O_BINARY)
#endif

#include "fatal_trace.h"

int host_player_main(int, char **);
typedef struct session_log {
    int read_fd, original;
    FILE *file;
    char error[4096];
    size_t length;
} session_log_t;

static int copy_stderr(void *data) {
    session_log_t *s=data;
    char bytes[4096];
    int n;
    while ((n=read(s->read_fd,bytes,sizeof(bytes)))>0) {
        size_t keep=(size_t)n;
        if (keep>sizeof(s->error)-1-s->length) keep=sizeof(s->error)-1-s->length;
        memcpy(s->error+s->length,bytes,keep); s->length+=keep; s->error[s->length]=0;
        fwrite(bytes,1,(size_t)n,s->file); fflush(s->file);
        /* A GUI process may have no inherited stderr handle. */
        for (int at=0, written; s->original>=0 && at<n; at+=written) {
            written=write(s->original,bytes+at,(unsigned)(n-at));
            if (written<=0) break;
        }
    }
    close(s->read_fd); return 0;
}
static int startup_error(const char *message, int headless) {
    fprintf(stderr,"%s\n",message);
    if (!headless) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Lionheart",message,NULL);
    return 2;
}
static int player_entry(int argc, char **argv) {
    char directory[4096], path[8192], from[8192], to[8192];
    session_log_t s={0};
    SDL_Thread *thread;
    int fds[2], result, headless=0, error_fd=fileno(stderr);
    for (int i=1;i<argc;++i)
        if (!strcmp(argv[i],"--headless") || !strcmp(argv[i],"--gui-test")) headless=1;
    fatal_trace_install(error_fd);
    /* Explicit card/state paths are sufficient without a discoverable data
     * directory. The player reports missing defaults after parsing its CLI. */
    if (!host_player_data(directory,sizeof(directory))) return host_player_main(argc,argv);
    snprintf(path,sizeof(path),"%s/logs",directory);
    if (!host_player_directory(path)) return startup_error("Cannot create Lionheart log directory",headless);
    for (int i=4;i>=1;--i) {
        snprintf(to,sizeof(to),"%s/logs/lionheart.%d.log",directory,i);
        if (i==1) snprintf(from,sizeof(from),"%s/logs/lionheart.log",directory);
        else snprintf(from,sizeof(from),"%s/logs/lionheart.%d.log",directory,i-1);
        remove(to); rename(from,to);
    }
    snprintf(path,sizeof(path),"%s/logs/lionheart.log",directory);
    s.file=fopen(path,"wb");
    if (!s.file || pipe(fds)) { if (s.file) fclose(s.file); return startup_error("Cannot open Lionheart session log",headless); }
    fatal_trace_install(fileno(s.file));
    fflush(stderr);
    s.original=dup(error_fd); s.read_fd=fds[0];
    if (dup2(fds[1],error_fd)<0) { fatal_trace_install(error_fd); close(fds[0]); close(fds[1]); fclose(s.file); if (s.original>=0) close(s.original); return startup_error("Cannot redirect Lionheart stderr",headless); }
    close(fds[1]);
#ifdef _WIN32
    SetStdHandle(STD_ERROR_HANDLE,(HANDLE)_get_osfhandle(error_fd));
#endif
    thread=SDL_CreateThread(copy_stderr,"session-log",&s);
    if (!thread) {
        close(error_fd); close(s.read_fd);
        fatal_trace_install(s.original); fclose(s.file);
        if (s.original>=0) { dup2(s.original,error_fd); close(s.original); }
#ifdef _WIN32
        SetStdHandle(STD_ERROR_HANDLE,s.original>=0 ? (HANDLE)_get_osfhandle(error_fd) : NULL);
#endif
        return startup_error("Cannot start Lionheart session log worker",headless);
    }
    result=host_player_main(argc,argv);
    fflush(stderr); close(error_fd); SDL_WaitThread(thread,NULL);
    if (s.original>=0) { dup2(s.original,error_fd); close(s.original); }
    fatal_trace_install(error_fd);
    fclose(s.file);
#ifdef _WIN32
    SetStdHandle(STD_ERROR_HANDLE,s.original>=0 ? (HANDLE)_get_osfhandle(error_fd) : NULL);
#endif
    if (result==2 && !headless && s.length)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Lionheart",s.error,NULL);
    return result;
}
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show) {
    /* Explorer and detached processes have FILE descriptors of -2. Reopen the
     * streams before tee setup; changing descriptor 2 alone cannot fix FILE*. */
    if (fileno(stdin)<0) freopen("NUL","r",stdin);
    if (fileno(stdout)<0) freopen("NUL","w",stdout);
    if (fileno(stderr)<0) freopen("NUL","w",stderr);
    int argc=0, result=2;
    wchar_t **wide=CommandLineToArgvW(GetCommandLineW(),&argc);
    char **argv=calloc((size_t)argc+1,sizeof(*argv));
    (void)instance; (void)previous; (void)command; (void)show;
    if (!wide || !argv) goto done;
    for (int i=0;i<argc;++i) {
        int n=WideCharToMultiByte(CP_UTF8,0,wide[i],-1,NULL,0,NULL,NULL);
        argv[i]=malloc((size_t)n);
        if (!argv[i]) goto done;
        WideCharToMultiByte(CP_UTF8,0,wide[i],-1,argv[i],n,NULL,NULL);
        for (char *p=argv[i];*p;++p) if (*p=='\\') *p='/';
    }
    result=player_entry(argc,argv);
done:
    if (argv) { for (int i=0;i<argc;++i) free(argv[i]); free(argv); }
    if (wide) LocalFree(wide);
    return result;
}
#else
int main(int argc, char **argv) { return player_entry(argc,argv); }
#endif
