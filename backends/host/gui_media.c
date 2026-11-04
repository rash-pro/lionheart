#define _POSIX_C_SOURCE 200809L
#include "gui_internal.h"
#include "disc_id.h"
#include "player_paths.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define CARD_BYTES 131072u
static int card_read(const char *path,uint8_t data[CARD_BYTES]) {
    FILE *file=fopen(path,"rb"); int ok;
    if (!file) return 0;
    ok=fread(data,1,CARD_BYTES,file)==CARD_BYTES && fgetc(file)==EOF && !ferror(file);
    if (fclose(file)) ok=0;
    return ok;
}
int gui_card_summary(const char *path,gui_card_summary_t *summary) {
    uint8_t *data=malloc(CARD_BYTES); unsigned i; int ok;
    memset(summary,0,sizeof(*summary));
    if (!data) return 0;
    ok=card_read(path,data);
    if (ok) for (i=1;i<16;++i) {
        const uint8_t *frame=data+i*128;
        if ((frame[0]&0xf0)==0x50) ++summary->blocks_used;
        if (frame[0]==0x51 && !memcmp(frame+10,"BASLUS-0089",11) && frame[21]>='0' && frame[21]<='9') ++summary->ff8_saves;
    }
    free(data); return ok;
}
static int card_directory(const char *data,char *path,size_t capacity,char *error,size_t size) {
    int n;
    if (!data || !*data) { snprintf(error,size,"%s",host_gui_string("card.err.data")); return 0; }
    n=snprintf(path,capacity,"%s/cards",data);
    if (n<0 || (size_t)n>=capacity) { snprintf(error,size,"%s",host_gui_string("card.err.path")); return 0; }
    if (!host_player_directory(path)) { snprintf(error,size,"%s",host_gui_string("card.err.create")); return 0; }
    return 1;
}
/* Raw PS1 formatting belongs to this host file operation. Linking the emulated
 * card protocol here would pull CPU/device save-state code into GUI presenters. */
static void card_format(uint8_t *data) {
    memset(data,0xff,CARD_BYTES);
    memset(data,0,36u*128u);
    data[0]='M'; data[1]='C';
    for (unsigned frame=1;frame<16;++frame) {
        uint8_t *p=data+frame*128u;
        p[0]=0xa0; p[8]=p[9]=0xff;
    }
    for (unsigned frame=16;frame<36;++frame) memset(data+frame*128u,0xff,4);
    for (unsigned frame=0;frame<36;++frame)
        for (unsigned i=0;i<127;++i) data[frame*128u+127]^=data[frame*128u+i];
    memcpy(data+63u*128u,data,128);
}
int gui_card_new(const char *data,unsigned slot,char *out,size_t capacity,char *error,size_t size) {
    char directory[4096]; uint8_t *bytes; FILE *file; int n,fd,ok;
    if (!card_directory(data,directory,sizeof(directory),error,size)) return 0;
    n=snprintf(out,capacity,"%s/card%u.mcr",directory,slot+1);
    if (n<0 || (size_t)n>=capacity) { snprintf(error,size,"%s",host_gui_string("card.err.path")); return 0; }
    bytes=malloc(CARD_BYTES);
    if (!bytes) { snprintf(error,size,"%s",host_gui_string("disc.err.memory")); return 0; }
    fd=-1;
    /* A pane's New card action must not reselect or truncate an attached card.
     * The setup default still uses card1.mcr when that filename is unused. */
    for (unsigned suffix=1;suffix<10000;++suffix) {
        n=suffix==1 ? snprintf(out,capacity,"%s/card%u.mcr",directory,slot+1) :
                     snprintf(out,capacity,"%s/card%u-%u.mcr",directory,slot+1,suffix);
        if (n<0 || (size_t)n>=capacity) { free(bytes); snprintf(error,size,"%s",host_gui_string("card.err.path")); return 0; }
        fd=open(out,O_WRONLY|O_CREAT|O_EXCL
#ifdef _WIN32
                |O_BINARY
#endif
                ,0600);
        if (fd>=0 || errno!=EEXIST) break;
    }
    if (fd<0) { free(bytes); snprintf(error,size,"%s",host_gui_string("card.err.create")); return 0; }
    card_format(bytes);
    file=fdopen(fd,"wb");
    if (!file) { close(fd); remove(out); free(bytes); snprintf(error,size,"%s",host_gui_string("card.err.create")); return 0; }
    ok=fwrite(bytes,1,CARD_BYTES,file)==CARD_BYTES && fflush(file)==0;
    if (fclose(file)) ok=0;
    free(bytes);
    if (!ok) { remove(out); snprintf(error,size,"%s",host_gui_string("card.err.create")); }
    return ok;
}
int gui_card_import(const char *source,const char *data,char *out,size_t capacity,char *error,size_t size) {
    uint8_t *bytes=malloc(CARD_BYTES); char directory[4096]; const char *base; int fd=-1,ok=0,n;
    FILE *file=NULL;
    if (!bytes) { snprintf(error,size,"%s",host_gui_string("disc.err.memory")); return 0; }
    if (!card_read(source,bytes)) { snprintf(error,size,"%s",host_gui_string("card.invalid")); goto done; }
    if (!card_directory(data,directory,sizeof(directory),error,size)) goto done;
    base=strrchr(source,'/'); base=base ? base+1 : source;
#ifdef _WIN32
    { const char *win=strrchr(base,'\\'); if (win) base=win+1; }
#endif
    if (!*base || !strcmp(base,".") || !strcmp(base,"..")) goto copy_error;
    n=snprintf(out,capacity,"%s/%s",directory,base);
    if (n<0 || (size_t)n>=capacity) { snprintf(error,size,"%s",host_gui_string("card.err.path")); goto done; }
    if (!strcmp(out,source)) { ok=1; goto done; }
    /* Never overwrite an existing card when importing. A collision gets a suffix. */
    for (unsigned suffix=1;suffix<10000;++suffix) {
        if (suffix>1) {
            n=snprintf(out,capacity,"%s/%u-%s",directory,suffix,base);
            if (n<0 || (size_t)n>=capacity) goto copy_error;
        }
        fd=open(out,O_WRONLY|O_CREAT|O_EXCL
#ifdef _WIN32
                |O_BINARY
#endif
                ,0600);
        if (fd>=0 || errno!=EEXIST) break;
    }
    if (fd<0) goto copy_error;
    file=fdopen(fd,"wb");
    if (!file) { close(fd); remove(out); goto copy_error; }
    ok=fwrite(bytes,1,CARD_BYTES,file)==CARD_BYTES && fflush(file)==0;
    if (fclose(file)) ok=0;
    if (!ok) { remove(out); goto copy_error; }
    goto done;
copy_error:
    snprintf(error,size,"%s",host_gui_string("card.err.copy"));
done:
    free(bytes); return ok;
}

/* The worker owns all job data. Only the GUI thread applies immutable results. */
#define QUEUE_CAPACITY 256
static struct {
    SDL_Thread *thread; SDL_mutex *mutex; SDL_cond *condition;
    struct { char path[512]; unsigned generation; int full,scan; } jobs[QUEUE_CAPACITY];
    gui_disc_result_t results[QUEUE_CAPACITY];
    unsigned job_read,job_write,result_read,result_write; SDL_atomic_t stop;
} media;
static int deliver(gui_disc_result_t *result) {
    SDL_LockMutex(media.mutex);
    while (!SDL_AtomicGet(&media.stop) && media.result_write-media.result_read==QUEUE_CAPACITY) SDL_CondWait(media.condition,media.mutex);
    if (!SDL_AtomicGet(&media.stop)) media.results[media.result_write++%QUEUE_CAPACITY]=*result;
    SDL_UnlockMutex(media.mutex); return !SDL_AtomicGet(&media.stop);
}
static void identify(const char *path,int full,unsigned generation,int scan) {
    gui_disc_result_t result; uint64_t start=SDL_GetPerformanceCounter(); struct stat st;
    memset(&result,0,sizeof(result));
    snprintf(result.path,sizeof(result.path),"%s",path); result.full=full; result.generation=generation; result.scan=scan;
    if (host_disc_identify(path,full,&result.disc,result.detail,sizeof(result.detail))) result.state=GUI_DISC_VERIFIED;
    else result.state=stat(path,&st) && errno==ENOENT ? GUI_DISC_MISSING : GUI_DISC_ERROR;
    result.elapsed=(double)(SDL_GetPerformanceCounter()-start)/(double)SDL_GetPerformanceFrequency();
    deliver(&result);
}
static int worker(void *unused) {
    gui_browser_t *scan_browser=NULL; unsigned scan_index=0,scan_generation=0;
    (void)unused;
    for (;;) {
        char path[512]; unsigned generation; int full,scan;
        SDL_LockMutex(media.mutex);
        while (!SDL_AtomicGet(&media.stop) && media.job_read==media.job_write &&
               (!scan_browser || scan_index==scan_browser->count)) SDL_CondWait(media.condition,media.mutex);
        if (SDL_AtomicGet(&media.stop)) { SDL_UnlockMutex(media.mutex); break; }
        if (media.job_read!=media.job_write) {
            unsigned index=media.job_read++%QUEUE_CAPACITY;
            memcpy(path,media.jobs[index].path,sizeof(path)); generation=media.jobs[index].generation;
            full=media.jobs[index].full; scan=media.jobs[index].scan;
        } else {
            const gui_browser_entry_t *entry=scan_browser->entries+scan_index++;
            if (entry->directory) { SDL_UnlockMutex(media.mutex); continue; }
            memcpy(path,entry->path,sizeof(path)); generation=scan_generation; full=0; scan=1;
        }
        SDL_UnlockMutex(media.mutex);
        if (scan==2) {
            free(scan_browser); scan_browser=calloc(1,sizeof(*scan_browser)); scan_index=0; scan_generation=generation;
            if (scan_browser && !gui_browser_open(scan_browser,path,GUI_PICK_DISC)) { free(scan_browser); scan_browser=NULL; }
        } else identify(path,full,generation,scan);
    }
    free(scan_browser); return 0;
}
int gui_media_start(void) {
    if (media.thread) return 1;
    memset(&media,0,sizeof(media));
    media.mutex=SDL_CreateMutex(); media.condition=SDL_CreateCond();
    if (media.mutex && media.condition) media.thread=SDL_CreateThread(worker,"gui-media",NULL);
    if (!media.thread) { gui_media_stop(); return 0; }
    return 1;
}
void gui_media_stop(void) {
    if (media.thread) { SDL_LockMutex(media.mutex); SDL_AtomicSet(&media.stop,1); SDL_CondBroadcast(media.condition); SDL_UnlockMutex(media.mutex); SDL_WaitThread(media.thread,NULL); }
    if (media.condition) SDL_DestroyCond(media.condition);
    if (media.mutex) SDL_DestroyMutex(media.mutex);
    memset(&media,0,sizeof(media));
}
int gui_media_identify(const char *path,int full,unsigned generation,int scan) {
    if (!media.thread || strlen(path)>=sizeof(media.jobs[0].path)) return 0;
    SDL_LockMutex(media.mutex);
    if (media.job_write-media.job_read==QUEUE_CAPACITY) { SDL_UnlockMutex(media.mutex); return 0; }
    unsigned index=media.job_write++%QUEUE_CAPACITY;
    strcpy(media.jobs[index].path,path); media.jobs[index].generation=generation; media.jobs[index].full=full; media.jobs[index].scan=scan;
    SDL_CondSignal(media.condition); SDL_UnlockMutex(media.mutex); return 1;
}
int gui_media_scan(const char *folder,unsigned generation) { return gui_media_identify(folder,0,generation,2); }
int gui_media_result(gui_disc_result_t *out) {
    int have;
    if (!media.mutex) return 0;
    SDL_LockMutex(media.mutex); have=media.result_read!=media.result_write;
    if (have) *out=media.results[media.result_read++%QUEUE_CAPACITY];
    SDL_CondSignal(media.condition); SDL_UnlockMutex(media.mutex); return have;
}
const char *gui_disc_error_id(const char *error) {
    if (strstr(error,"No such file") || strstr(error,"not found")) return "disc.err.missing";
    if (strstr(error,"unsupported image")) return "disc.err.unsupported";
    if (strstr(error,"not a PlayStation")) return "disc.err.not_psx";
    if (strstr(error,"Another game")) return "disc.err.other_game";
    if (strstr(error,"Only the USA")) return "disc.err.region";
    if (strstr(error,"out of memory") || !SDL_strncasecmp(error,"Out of memory",13)) return "disc.err.memory";
    if (strstr(error,"IMG") && strstr(error,"SHA-1")) return "disc.err.img_hash";
    if (strstr(error,"EXE missing")) return "disc.err.exe_missing";
    if (strstr(error,"EXE") && (strstr(error,"SHA-1") || strstr(error,"FNV") || strstr(error,"header"))) return "disc.err.hash";
    if (strstr(error,"size") || strstr(error,"LBA") || strstr(error,"truncated")) return "disc.err.size";
    return "disc.err.read";
}
