#define _POSIX_C_SOURCE 200809L
#include "disc_id.h"
#define HOST_DISC_TABLE
#include "disc_identity.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sha1_stream.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
typedef struct {
    char binary[4096], mode[32];
    unsigned index;
    int normalize;
} disc_source_t;

static int cue_suffix(const char *path) {
    size_t n = strlen(path);
    /* Keep this rule identical to runtime/cdrom.c, including mixed case. */
    return n >= 4 && (!strcmp(path+n-4,".cue") || !strcmp(path+n-4,".CUE"));
}
static int source(const char *path, disc_source_t *out, char *error, size_t error_size) {
    char line[4096], name[3072], mode[32];
    FILE *file;
    size_t n = strlen(path);
    const char *failed_path = path;
#ifdef _WIN32
    char normalized[4096];
#endif
    memset(out,0,sizeof(*out));
    if (n >= sizeof(out->binary) || n < 4) goto invalid;
#ifdef _WIN32
    strcpy(normalized,path);
    for (char *p=normalized;*p;++p) if (*p=='\\') *p='/';
    path = normalized;
#endif
    strcpy(out->binary,path);
    if (cue_suffix(path)) {
        unsigned files = 0, tracks = 0, found_index = 0, first = 1;
        file = fopen(path,"r"); if (!file) goto io_error;
        while (fgets(line,sizeof(line),file)) {
            unsigned track, m, s, f;
            char *text = line;
            if (first && strlen(line) >= 3 && !memcmp(line,"\xef\xbb\xbf",3)) {
                text += 3; out->normalize = 1;
            }
            first = 0;
            if (sscanf(text," FILE \"%3071[^\"]\" %31s",name,mode) == 2) {
                const char *slash = strrchr(path,'/');
                size_t prefix = slash ? (size_t)(slash - path + 1) : 0;
                if (++files != 1 || strcmp(mode,"BINARY") || prefix + strlen(name) >= sizeof(out->binary)) { fclose(file); goto invalid; }
#ifdef _WIN32
                if (name[0]=='\\' || (name[0] && name[1]==':')) out->normalize=1;
                for (char *p=name;*p;++p) if (*p=='\\') *p='/';
                if (name[0]=='/' || (name[0] && name[1]==':')) strcpy(out->binary,name);
#else
                if (name[0] == '/') strcpy(out->binary,name);
#endif
                else { memcpy(out->binary,path,prefix); strcpy(out->binary + prefix,name); }
            } else if (sscanf(text," TRACK %u %31s",&track,mode) == 2) {
                if (++tracks != 1 || track != 1 || (strcmp(mode,"MODE2/2352") && strcmp(mode,"MODE1/2352"))) { fclose(file); goto invalid; }
                strcpy(out->mode,mode);
            } else if (sscanf(text," INDEX 01 %u:%u:%u",&m,&s,&f) == 3) {
                if (++found_index != 1 || s >= 60 || f >= 75 || m > 99) { fclose(file); goto invalid; }
                out->index = (m * 60 + s) * 75 + f;
            }
        }
        n = ferror(file); fclose(file);
        if (n || files != 1 || tracks != 1 || !found_index) goto invalid;
    } else if (strcasecmp(path+n-4,".bin")) goto invalid;
    return 1;
io_error:
    snprintf(error,error_size,"%s: %s; supported: single-track .cue with MODE2/2352 or MODE1/2352, or raw 2352-byte .bin",failed_path,strerror(errno)); return 0;
invalid:
    snprintf(error,error_size,"%s: unsupported image; supported: single-track .cue with MODE2/2352 or MODE1/2352, or raw 2352-byte .bin",path); return 0;
}
int host_disc_open(const char *path, hw_cd_t *d, char *error, size_t error_size) {
    disc_source_t src;
    struct stat st;
    FILE *file;
    memset(d,0,sizeof(*d));
    if (!source(path,&src,error,error_size)) return 0;
    file = fopen(src.binary,"rb");
    if (!file) {
        snprintf(error,error_size,"%s: %s; supported: single-track .cue with MODE2/2352 or MODE1/2352, or raw 2352-byte .bin",src.binary,strerror(errno));
        return 0;
    }
    if (fstat(fileno(file),&st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size % 2352 ||
        (uint64_t)st.st_size / 2352 > UINT32_MAX || (uint64_t)st.st_size / 2352 <= src.index) {
        fclose(file);
        snprintf(error,error_size,"%s: unsupported image; supported: single-track .cue with MODE2/2352 or MODE1/2352, or raw 2352-byte .bin",src.binary);
        return 0;
    }
    d->file = file; d->offset = src.index; d->sectors = (uint32_t)(st.st_size / 2352) - src.index;
    return 1;
}
static int absolute_binary(const char *path, char out[3072]) {
#ifdef _WIN32
    wchar_t input[4096], absolute[4096];
    char utf8[4096];
    DWORD n;
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,input,4096)) return 0;
    n = GetFullPathNameW(input,4096,absolute,NULL);
    if (!n || n >= 4096 || !WideCharToMultiByte(CP_UTF8,0,absolute,-1,utf8,sizeof(utf8),NULL,NULL)) return 0;
    for (char *p=utf8;*p;++p) if (*p=='\\') *p='/';
    /* runtime's CUE parser recognizes leading / as absolute. */
    if (!strncmp(utf8,"//?/",4)) n = (DWORD)snprintf(out,3072,"%s",utf8);
    else if (!strncmp(utf8,"//",2)) n = (DWORD)snprintf(out,3072,"//?/UNC/%s",utf8+2);
    else n = (DWORD)snprintf(out,3072,"//?/%s",utf8);
    return n < 3072;
#else
    char cwd[4096];
    int n;
    if (*path == '/') n = snprintf(out,3072,"%s",path);
    else {
        if (!getcwd(cwd,sizeof(cwd))) return 0;
        n = snprintf(out,3072,"%s/%s",cwd,path);
    }
    return n >= 0 && n < 3072;
#endif
}
int host_disc_runtime_path(const char *path, host_disc_runtime_t *out, char *error, size_t error_size) {
    disc_source_t src;
    char absolute[3072], directory[4096];
    const char *temp;
    FILE *file;
    int n, fd;
    memset(out,0,sizeof(*out));
    n = snprintf(out->path,sizeof(out->path),"%s",path);
    if (n < 0 || (size_t)n >= sizeof(out->path)) {
        snprintf(error,error_size,"Disc path exceeds %u bytes",(unsigned)sizeof(out->path)-1); return 0;
    }
#ifdef _WIN32
    for (char *p=out->path;*p;++p) if (*p=='\\') *p='/';
#endif
    if (!cue_suffix(path)) return 1;
    if (!source(path,&src,error,error_size)) return 0;
    if (!src.normalize) return 1;
    if (!absolute_binary(src.binary,absolute) || strpbrk(absolute,"\"\r\n")) {
        snprintf(error,error_size,"Cannot normalize CUE BIN path (maximum 3071 bytes): %s",src.binary); return 0;
    }
#ifdef _WIN32
    temp = getenv("TEMP"); if (!temp || !*temp) temp = getenv("TMP");
    if (!temp || !*temp) temp = ".";
#else
    temp = getenv("TMPDIR"); if (!temp || !*temp) temp = "/tmp";
#endif
    n = snprintf(directory,sizeof(directory),"%s/lionheart-cue-XXXXXX",temp);
    if (n < 0 || (size_t)n >= sizeof(directory)-10) {
        snprintf(error,error_size,"Temporary CUE path exceeds 4095 bytes"); return 0;
    }
#ifdef _WIN32
    if (_mktemp_s(directory,sizeof(directory)) || mkdir(directory,0700)) {
#else
    if (!mkdtemp(directory)) {
#endif
        snprintf(error,error_size,"Cannot create temporary CUE directory: %s",strerror(errno)); return 0;
    }
    memcpy(out->path,directory,(size_t)n);
    strcpy(out->path+n,"/disc.cue");
    fd = open(out->path,O_WRONLY|O_CREAT|O_EXCL,0600);
    if (fd < 0) {
        snprintf(error,error_size,"Cannot create temporary CUE: %s",strerror(errno));
        rmdir(directory); out->path[0] = 0; return 0;
    }
    out->temporary = 1;
    file = fdopen(fd,"w");
    if (!file) {
        close(fd);
        snprintf(error,error_size,"Cannot create temporary CUE stream: %s",strerror(errno));
        host_disc_runtime_cleanup(out); return 0;
    }
    n = fprintf(file,"FILE \"%s\" BINARY\n TRACK 01 %s\n INDEX 01 %02u:%02u:%02u\n",absolute,src.mode,
                src.index/(60*75),src.index/75%60,src.index%75);
    if (fclose(file) || n < 0) {
        snprintf(error,error_size,"Cannot write temporary CUE: %s",strerror(errno));
        host_disc_runtime_cleanup(out); return 0;
    }
    return 1;
}
void host_disc_runtime_cleanup(host_disc_runtime_t *path) {
    if (path->temporary) {
        char *slash;
        remove(path->path);
        slash = strrchr(path->path,'/');
        if (slash) { *slash = 0; rmdir(path->path); }
    }
    memset(path,0,sizeof(*path));
}
void host_disc_sanitize(char *out, size_t size, const char *input) {
    size_t i;
    if (!size) return;
    for (i=0;i+1<size && input[i];++i) {
        unsigned char c = (unsigned char)input[i];
        out[i] = ((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') ||
                  c=='_' || c=='.' || c=='-') ? (char)c : '?';
    }
    out[i] = 0;
}
static int sha1_matches(const uint8_t *data, size_t size, const char *expected) {
    uint8_t hash[20]; char text[41]; unsigned i; host_sha1_t stream;
    host_sha1_init(&stream); host_sha1_update(&stream,data,size); host_sha1_final(&stream,hash);
    for (i = 0; i < 20; ++i) snprintf(text + i*2,3,"%02x",hash[i]);
    return !strcmp(text,expected);
}
int host_exe_check(const uint8_t *data, size_t size, char *error, size_t error_size) {
    uint32_t i, hash = 2166136261u;
    const char *failed = NULL;
    if (size != HOST_EXE_FILE_SIZE) failed = "EXE file size";
    else if (memcmp(data,"PS-X EXE",8)) failed = "EXE header";
    else if (le32(data+24) != HOST_EXE_BASE || le32(data+28) != HOST_EXE_SIZE) failed = "EXE text range/size";
    else {
        for (i=0;i<HOST_EXE_SIZE;++i) hash = (hash ^ data[2048+i]) * 16777619u;
        if (hash != HOST_EXE_FNV) failed = "EXE FNV-1a";
        else if (!sha1_matches(data,size,HOST_EXE_SHA1)) failed = "EXE SHA-1";
    }
    if (failed) snprintf(error,error_size,"FF8 USA %s check failed (expected SHA-1 %s)",failed,HOST_EXE_SHA1);
    return !failed;
}
static int read_exe(const hw_cd_t *d, const char *name, uint8_t **data, size_t *size, char *error, size_t error_size) {
    uint32_t lba, bytes;
    char safe[64];
    host_disc_sanitize(safe,sizeof(safe),name);
    *data = NULL; *size = 0;
    if (!host_iso_find(d,name,&lba,&bytes)) {
        snprintf(error,error_size,"FF8 USA EXE missing (%s)",safe); return 0;
    }
    if (bytes != HOST_EXE_FILE_SIZE) {
        snprintf(error,error_size,"FF8 USA EXE size check failed (%s)",safe); return 0;
    }
    *data = malloc(bytes);
    if (!*data) { snprintf(error,error_size,"Out of memory reading EXE %s",safe); return 0; }
    if (!host_iso_read(d,lba,bytes,*data)) {
        free(*data); *data = NULL; snprintf(error,error_size,"Cannot read EXE %s",safe); return 0;
    }
    *size = bytes; return 1;
}
int host_disc_exe(const char *path, uint8_t **data, size_t *size, char *error, size_t error_size) {
    hw_cd_t d; char name[64]; int ok;
    if (!host_disc_open(path,&d,error,error_size)) return 0;
    ok = host_disc_boot(&d,name);
    if (!ok) snprintf(error,error_size,"%s: not a PlayStation disc (missing SYSTEM.CNF BOOT)",path);
    else ok = read_exe(&d,name,data,size,error,error_size);
    fclose(d.file); return ok;
}
int host_disc_identify(const char *path, int full, unsigned *number, char *error, size_t error_size) {
    hw_cd_t d; char name[64], safe[64]; uint8_t *data = NULL;
    uint32_t lba, bytes; size_t size; unsigned i; int ok = 0, ff8 = 0;
    const host_disc_metadata_t *m;
    if (!host_disc_open(path,&d,error,error_size)) return 0;
    if (!host_disc_boot(&d,name)) { snprintf(error,error_size,"%s: not a PlayStation disc (missing SYSTEM.CNF BOOT)",path); goto done; }
    host_disc_sanitize(safe,sizeof(safe),name);
    if (strpbrk(name,"/\\")) { snprintf(error,error_size,"Another game: %s",safe); goto done; }
    for (i=0;i<4;++i) {
        if (!strcasecmp(name,host_disc_metadata[i].serial)) break;
    }
    if (i == 4) {
        for (i=0;i<4;++i) ff8 |= host_iso_find(&d,host_disc_metadata[i].image,&lba,&bytes);
        if (ff8) snprintf(error,error_size,"Only the USA release of Final Fantasy VIII (SLUS-00892/00908/00909/00910) is supported");
        else snprintf(error,error_size,"Another game: %s",safe);
        goto done;
    }
    m = host_disc_metadata + i;
    if (!read_exe(&d,name,&data,&size,error,error_size)) goto done;
    if (!host_exe_check(data,size,error,error_size)) goto done;
    if (!sha1_matches(data,size,m->exe_sha1)) { snprintf(error,error_size,"FF8 USA disc %u EXE SHA-1 check failed",i+1); goto done; }
    free(data); data = NULL;
    if (!host_iso_find(&d,m->image,&lba,&bytes)) { snprintf(error,error_size,"FF8 USA disc %u %s missing or truncated (size check failed)",i+1,m->image); goto done; }
    if (lba != m->image_lba) { snprintf(error,error_size,"FF8 USA disc %u %s LBA check failed (expected %u)",i+1,m->image,m->image_lba); goto done; }
    if (bytes != m->image_size) { snprintf(error,error_size,"FF8 USA disc %u %s size check failed (expected %u)",i+1,m->image,m->image_size); goto done; }
    if (full) {
        host_sha1_t stream; uint8_t sector[2048], digest[20]; char hex[41];
        host_sha1_init(&stream);
        for (uint32_t pos=0;pos<bytes;) {
            unsigned n=bytes-pos<2048 ? bytes-pos : 2048;
            if (!host_iso_sector(&d,lba++,sector)) { snprintf(error,error_size,"Cannot read %s for full verification",m->image); goto done; }
            host_sha1_update(&stream,sector,n); pos+=n;
        }
        host_sha1_final(&stream,digest);
        for (unsigned j=0;j<20;++j) snprintf(hex+j*2,3,"%02x",digest[j]);
        if (strcmp(hex,m->image_sha1)) { snprintf(error,error_size,"FF8 USA disc %u %s SHA-1 check failed",i+1,m->image); goto done; }
    }
    *number = i+1; ok = 1;
done:
    free(data); fclose(d.file); return ok;
}
int host_disc_assign(const char **slots, const char *path, int full, char *error, size_t error_size) {
    unsigned disc;
    if (!host_disc_identify(path,full,&disc,error,error_size)) return 0;
    if (slots[disc-1]) { snprintf(error,error_size,"Two images for FF8 USA disc %u",disc); return 0; }
    slots[disc-1] = path; return 1;
}
