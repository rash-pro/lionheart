#define _POSIX_C_SOURCE 200809L
#include "identity.h"
#include "screenshot.h"
#include "osd.h"
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

static void big_endian(uint8_t *out, uint32_t n) {
    out[0] = (uint8_t)(n >> 24); out[1] = (uint8_t)(n >> 16); out[2] = (uint8_t)(n >> 8); out[3] = (uint8_t)n;
}

static uint32_t png_crc(const uint8_t *p, size_t size, uint32_t crc) {
    uint32_t table[256];
    {
        unsigned i, j;
        for (i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (j = 0; j < 8; ++j) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
            table[i] = c;
        }
    }
    while (size--) crc = table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return crc;
}

static int chunk(FILE *file, const char *type, const uint8_t *data, size_t size) {
    uint8_t bytes[4];
    uint32_t crc = png_crc((const uint8_t *)type, 4, 0xffffffffu);
    big_endian(bytes, (uint32_t)size);
    if (fwrite(bytes, 1, 4, file) != 4 || fwrite(type, 1, 4, file) != 4) return 0;
    if (size && fwrite(data, 1, size, file) != size) return 0;
    crc = png_crc(data, size, crc);
    big_endian(bytes, ~crc);
    return fwrite(bytes, 1, 4, file) == 4;
}

/* PNG RGB8 with filter None and zlib stored blocks; no external encoder or compressed assets. */
int host_png(const char *path, const uint8_t *rgb, int w, int h) {
    static const uint8_t signature[8] = {137,80,78,71,13,10,26,10};
    uint8_t header[13] = {0}, *raw, *z;
    size_t stride, size, blocks, pos = 2, at = 0, i;
    uint32_t a = 1, b = 0;
    int y, ok;
    FILE *file;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    stride = (size_t)w * 3 + 1; size = stride * (size_t)h;
    blocks = (size + 65534) / 65535;
    raw = malloc(size); z = malloc(size + blocks * 5 + 6);
    if (!raw || !z) { free(raw); free(z); return 0; }
    for (y = 0; y < h; ++y) {
        raw[(size_t)y * stride] = 0;
        memcpy(raw + (size_t)y * stride + 1, rgb + (size_t)y * w * 3, (size_t)w * 3);
    }
    z[0] = 0x78; z[1] = 0x01;
    while (at < size) {
        size_t n = size - at < 65535 ? size - at : 65535;
        z[pos++] = at + n == size ? 1 : 0;
        z[pos++] = (uint8_t)n; z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)~n; z[pos++] = (uint8_t)(~n >> 8);
        memcpy(z + pos, raw + at, n); pos += n; at += n;
    }
    for (i = 0; i < size; ++i) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    big_endian(z + pos, (b << 16) | a); pos += 4;
    free(raw);
    file = fopen(path, "wb");
    if (!file) { free(z); return 0; }
    big_endian(header, (uint32_t)w); big_endian(header + 4, (uint32_t)h);
    header[8] = 8; header[9] = 2;
    ok = fwrite(signature, 1, 8, file) == 8 && chunk(file, "IHDR", header, 13)
        && chunk(file, "IDAT", z, pos) && chunk(file, "IEND", NULL, 0);
    free(z);
    if (fclose(file)) ok = 0;
    return ok;
}

/* Screenshots: filter Up on every row and zlib level 6, about a tenth of the
 * stored size at 4K; only the worker thread pays for the compression. */
static int png_compressed(const char *path, const uint8_t *rgb, int w, int h) {
    static const uint8_t signature[8] = {137,80,78,71,13,10,26,10};
    uint8_t header[13] = {0}, *raw, *z;
    size_t row = (size_t)w * 3, stride = row + 1, i;
    uLongf zsize;
    int y, ok;
    FILE *file;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    raw = malloc(stride * (size_t)h); zsize = compressBound((uLong)(stride * (size_t)h)); z = malloc(zsize);
    if (!raw || !z) { free(raw); free(z); return 0; }
    for (y = 0; y < h; ++y) {
        const uint8_t *cur = rgb + (size_t)y * row;
        uint8_t *out = raw + (size_t)y * stride;
        out[0] = 2;
        if (y) for (i = 0; i < row; ++i) out[i + 1] = (uint8_t)(cur[i] - cur[i - row]);
        else memcpy(out + 1, cur, row);
    }
    ok = compress2(z, &zsize, raw, (uLong)(stride * (size_t)h), 6) == Z_OK;
    free(raw);
    file = ok ? fopen(path, "wb") : NULL;
    if (!file) { free(z); return 0; }
    big_endian(header, (uint32_t)w); big_endian(header + 4, (uint32_t)h);
    header[8] = 8; header[9] = 2;
    ok = fwrite(signature, 1, 8, file) == 8 && chunk(file, "IHDR", header, 13)
        && chunk(file, "IDAT", z, zsize) && chunk(file, "IEND", NULL, 0);
    free(z);
    if (fclose(file)) ok = 0;
    return ok;
}

static struct {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *wake;
    int state, stop, w, h, format; /* idle, requested, submitted, completed */
    void *data;
    const uint8_t *(*read)(void *);
    void (*release)(void *);
    char directory[4096], reason[64];
    uint64_t frames[32];
    unsigned count;
} shot;

/* user-dirs.dirs is data, never shell code. Expand only its documented $HOME. */
int host_screenshot_directory(char *out, unsigned size) {
#ifdef _WIN32
    PWSTR pictures = NULL;
    char utf8[32768];
    int n;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Pictures,0,NULL,&pictures))) return 0;
    n=WideCharToMultiByte(CP_UTF8,0,pictures,-1,utf8,sizeof(utf8),NULL,NULL);
    CoTaskMemFree(pictures);
    if (!n) return 0;
    for (char *p=utf8;*p;++p) if (*p=='\\') *p='/';
    n=snprintf(out,size,"%s/" HOST_PICTURES,utf8);
    return n>0 && (unsigned)n<size;
#else
    const char *pictures=getenv("XDG_PICTURES_DIR"), *home=getenv("HOME"), *config=getenv("XDG_CONFIG_HOME");
    char path[4096], line[8192], value[4096]="";
    int n;
    if (!home) home=".";
    if (!pictures || !*pictures) {
        FILE *f;
        if (config && *config) n=snprintf(path,sizeof(path),"%s/user-dirs.dirs",config);
        else n=snprintf(path,sizeof(path),"%s/.config/user-dirs.dirs",home);
        f=n>0 && (size_t)n<sizeof(path) ? fopen(path,"r") : NULL;
        while (f && fgets(line,sizeof(line),f)) {
            char *p=line, *q, *dest=value;
            while (*p==' ' || *p=='\t') ++p;
            if (strncmp(p,"XDG_PICTURES_DIR=",17)) continue;
            p+=17; if (*p++!='"') continue;
            q=p;
            while (*q && *q!='"' && (size_t)(dest-value)+1<sizeof(value)) {
                if (*q=='\\' && q[1]) ++q;
                *dest++=*q++;
            }
            *dest=0;
            if (*q!='"') *value=0;
        }
        if (f) fclose(f);
        pictures=value;
    }
    if (!strncmp(pictures,"$HOME",5) && (!pictures[5] || pictures[5]=='/'))
        n=snprintf(out,size,"%s%s/" HOST_PICTURES,home,pictures+5);
    else if (*pictures=='/') n=snprintf(out,size,"%s/" HOST_PICTURES,pictures);
    else n=snprintf(out,size,"%s/Pictures/" HOST_PICTURES,home);
    return n>0 && (unsigned)n<size;
#endif
}
static int directory(const char *path) {
    char copy[4096]; size_t i,n=strlen(path); struct stat st;
    if (!n || n>=sizeof(copy)) return 0;
    memcpy(copy,path,n+1);
    for (i=1;i<=n;++i) if (copy[i]=='/' || !copy[i]) {
        char saved=copy[i]; copy[i]=0;
        if ((mkdir(copy,0777) && errno!=EEXIST) || stat(copy,&st) || !S_ISDIR(st.st_mode)) return 0;
        copy[i]=saved;
    }
    return 1;
}
static int write_shot(const uint8_t *rgb,int w,int h) {
    char path[8192],stamp[32]; unsigned collision=1; time_t now=time(NULL); struct tm local;
    if (!directory(shot.directory) || !localtime_r(&now,&local) || !strftime(stamp,sizeof(stamp),HOST_SHOT_PREFIX "%Y%m%d-%H%M%S",&local)) return 0;
    for (;;) {
        int fd;
        if (collision==1) snprintf(path,sizeof(path),"%s/%s.png",shot.directory,stamp);
        else snprintf(path,sizeof(path),"%s/%s-%u.png",shot.directory,stamp,collision);
        fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0666);
        if (fd>=0) { close(fd); break; }
        if (errno!=EEXIST || ++collision==0) return 0;
    }
    if (!png_compressed(path,rgb,w,h)) { unlink(path); return 0; }
    fprintf(stderr,"Screenshot saved: %s\n",path); return 1;
}
static int worker(void *unused) {
    (void)unused;
    SDL_LockMutex(shot.mutex);
    for (;;) {
        const uint8_t *pixels; uint8_t *rgb=NULL; const char *reason=NULL;
        while (!shot.stop && shot.state!=2) SDL_CondWait(shot.wake,shot.mutex);
        if (shot.state!=2 && shot.stop) break;
        SDL_UnlockMutex(shot.mutex);
        pixels=shot.read(shot.data);
        if (!pixels) reason="cannot read";
        else {
            if (shot.format!=HOST_SCREENSHOT_RGB) {
                size_t i,n=(size_t)shot.w*shot.h;
                rgb=malloc(n*3);
                if (rgb) for (i=0;i<n;++i) {
                    rgb[i*3]=pixels[i*4+(shot.format==HOST_SCREENSHOT_BGRA ? 2 : 0)];
                    rgb[i*3+1]=pixels[i*4+1];
                    rgb[i*3+2]=pixels[i*4+(shot.format==HOST_SCREENSHOT_BGRA ? 0 : 2)];
                }
                else reason="out of memory";
            }
            if (!reason && !write_shot(rgb ? rgb : pixels,shot.w,shot.h)) reason="cannot write";
        }
        free(rgb);
        SDL_LockMutex(shot.mutex);
        snprintf(shot.reason,sizeof(shot.reason),"%s",reason ? reason : ""); shot.state=3;
    }
    SDL_UnlockMutex(shot.mutex); return 0;
}
int host_screenshot_configure(const char *dir,const char *frames) {
    char *end;
    if (shot.mutex) return 0;
    memset(&shot,0,sizeof(shot));
    if (dir && *dir) {
        if (strlen(dir)>=sizeof(shot.directory)) return 0;
        strcpy(shot.directory,dir);
    } else if (!host_screenshot_directory(shot.directory,sizeof(shot.directory))) return 0;
    while (frames && *frames && shot.count<32) {
        shot.frames[shot.count++]=strtoull(frames,&end,10);
        frames=*end ? end+1 : end;
    }
    shot.mutex=SDL_CreateMutex(); shot.wake=SDL_CreateCond();
    if (!shot.mutex || !shot.wake) { host_screenshot_shutdown(); return 0; }
    return 1;
}
void host_screenshot_request(void) {
    if (!shot.mutex && !host_screenshot_configure(NULL,NULL)) return;
    SDL_LockMutex(shot.mutex);
    if (!shot.state) shot.state=1;
    SDL_UnlockMutex(shot.mutex);
}
static void result(const char *reason) {
    host_osd_event(reason ? HOST_OSD_SCREENSHOT_FAILED : HOST_OSD_SCREENSHOT,0,reason);
    if (reason) fprintf(stderr,"Screenshot failed: %s\n",reason);
}
void host_screenshot_fail(const char *reason) {
    if (shot.mutex) {
        SDL_LockMutex(shot.mutex); shot.state=0; SDL_UnlockMutex(shot.mutex);
    }
    result(reason);
}
void host_screenshot_frame(uint64_t frame) {
    unsigned i;
    if (shot.mutex) {
        SDL_LockMutex(shot.mutex);
        if (shot.state==3) {
            shot.release(shot.data); shot.data=NULL;
            result(*shot.reason ? shot.reason : NULL);
            shot.state=0;
        }
        SDL_UnlockMutex(shot.mutex);
    }
    for (i=0;i<shot.count;++i) if (shot.frames[i]==frame) host_screenshot_request();
}
int host_screenshot_pending(void) {
    int pending=0;
    if (shot.mutex) {
        SDL_LockMutex(shot.mutex); pending=shot.state==1; SDL_UnlockMutex(shot.mutex);
    }
    return pending;
}
void host_screenshot_submit(int w,int h,int format,void *data,const uint8_t *(*read)(void *),void (*release)(void *)) {
    SDL_LockMutex(shot.mutex);
    shot.w=w; shot.h=h; shot.format=format; shot.data=data; shot.read=read; shot.release=release;
    shot.state=2;
    if (!shot.thread) shot.thread=SDL_CreateThread(worker,"ff8-screenshot",NULL);
    if (!shot.thread) {
        release(data); shot.state=0; result("cannot start worker");
    } else SDL_CondSignal(shot.wake);
    SDL_UnlockMutex(shot.mutex);
}
void host_screenshot_flush(void) {
    if (shot.mutex) {
        SDL_LockMutex(shot.mutex); shot.stop=1;
        if (shot.wake) SDL_CondSignal(shot.wake);
        SDL_UnlockMutex(shot.mutex);
        if (shot.thread) SDL_WaitThread(shot.thread,NULL);
        if (shot.state==3) {
            shot.release(shot.data); shot.data=NULL;
            result(*shot.reason ? shot.reason : NULL);
        }
    }
    shot.thread=NULL; shot.stop=0;
    if (shot.state==3) shot.state=0;
}
void host_screenshot_shutdown(void) {
    host_screenshot_flush();
    SDL_DestroyCond(shot.wake); SDL_DestroyMutex(shot.mutex); memset(&shot,0,sizeof(shot));
}
