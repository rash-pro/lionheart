#define HOST_WINDOWS_IO_IMPLEMENTATION
#include "windows_io.h"
#include <windows.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>

static wchar_t *wide(const char *text) {
    int n = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,NULL,0);
    wchar_t *w = n ? malloc((size_t)n * sizeof(*w)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,-1,w,n);
    else errno = EINVAL;
    return w;
}
/* Wide CRT calls plus extended absolute paths cover UTF-8 and paths > MAX_PATH,
 * including Wine versions which do not implement activeCodePage manifests. */
static wchar_t *path(const char *text) {
    wchar_t *w, *absolute, *out;
    if (!_stricmp(text,"NUL") || !strcmp(text,"/dev/null")) return wide("NUL");
    w = wide(text);
    DWORD n;
    if (!w) return NULL;
    n = GetFullPathNameW(w,0,NULL,NULL);
    absolute = n ? malloc((size_t)n * sizeof(*absolute)) : NULL;
    if (!absolute || !GetFullPathNameW(w,n,absolute,NULL)) { free(w); free(absolute); return NULL; }
    free(w);
    out = malloc(((size_t)n + 8) * sizeof(*out));
    if (out) {
        if (!wcsncmp(absolute,L"\\\\?\\",4)) wcscpy(out,absolute);
        else if (!wcsncmp(absolute,L"\\\\",2)) { wcscpy(out,L"\\\\?\\UNC\\"); wcscat(out,absolute+2); }
        else { wcscpy(out,L"\\\\?\\"); wcscat(out,absolute); }
    }
    free(absolute); return out;
}
FILE *host_win_fopen(const char *name, const char *mode) {
    wchar_t *p = path(name), *m = wide(mode);
    FILE *f = p && m ? _wfopen(p,m) : NULL;
    free(p); free(m); return f;
}
int host_win_open(const char *name, int flags, ...) {
    int mode = 0, fd; wchar_t *p = path(name);
    if (flags & O_CREAT) { va_list args; va_start(args,flags); mode = va_arg(args,int); va_end(args); }
    fd = p ? _wopen(p,flags,mode) : -1; free(p); return fd;
}
static __time64_t unix_time(FILETIME t) {
    ULARGE_INTEGER u; u.LowPart = t.dwLowDateTime; u.HighPart = t.dwHighDateTime;
    return u.QuadPart < 116444736000000000ULL ? 0 : (__time64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
/* The C runtime's stat refuses any path that contains '?', which every extended
 * path does, so the attributes come from the system. Opening the file follows
 * links as stat does; a file another process holds exclusively is still described. */
int host_win_stat(const char *name, struct _stat64 *st) {
    wchar_t *p = path(name); DWORD attributes, low, high; FILETIME created, accessed, written; HANDLE h;
    if (!p) return -1;
    h = CreateFileW(p,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if (h != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info; BOOL ok = GetFileInformationByHandle(h,&info);
        CloseHandle(h); free(p);
        if (!ok) { errno = EACCES; return -1; }
        attributes = info.dwFileAttributes; low = info.nFileSizeLow; high = info.nFileSizeHigh;
        created = info.ftCreationTime; accessed = info.ftLastAccessTime; written = info.ftLastWriteTime;
    } else {
        WIN32_FILE_ATTRIBUTE_DATA data; BOOL ok = GetFileAttributesExW(p,GetFileExInfoStandard,&data);
        DWORD error = GetLastError();
        free(p);
        if (!ok) { errno = error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ? EACCES : ENOENT; return -1; }
        attributes = data.dwFileAttributes; low = data.nFileSizeLow; high = data.nFileSizeHigh;
        created = data.ftCreationTime; accessed = data.ftLastAccessTime; written = data.ftLastWriteTime;
    }
    memset(st,0,sizeof(*st));
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) st->st_mode = _S_IFDIR | 0777;
    else st->st_mode = _S_IFREG | (attributes & FILE_ATTRIBUTE_READONLY ? 0444 : 0666);
    st->st_nlink = 1;
    st->st_size = (__int64)(((unsigned __int64)high << 32) | low);
    st->st_ctime = unix_time(created); st->st_atime = unix_time(accessed); st->st_mtime = unix_time(written);
    return 0;
}
int host_win_mkdir(const char *name) { wchar_t *p=path(name); int r=p ? _wmkdir(p) : -1; free(p); return r; }
int host_win_unlink(const char *name) { wchar_t *p=path(name); int r=p ? _wunlink(p) : -1; free(p); return r; }
int host_win_rename(const char *from, const char *to) {
    wchar_t *a=path(from), *b=path(to);
    int r=a && b && MoveFileExW(a,b,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH) ? 0 : -1;
    if (r) errno=EACCES;
    free(a); free(b); return r;
}
char *host_win_getenv(const char *name) {
    static struct entry { char *name, *value; struct entry *next; } *entries;
    struct entry *e; wchar_t *key=wide(name), *value=key ? _wgetenv(key) : NULL;
    int n=value ? WideCharToMultiByte(CP_UTF8,0,value,-1,NULL,0,NULL,NULL) : 0;
    free(key);
    for (e=entries;e && strcmp(e->name,name);e=e->next) {}
    if (!e) { e=calloc(1,sizeof(*e)); if (!e) return NULL; e->name=strdup(name); e->next=entries; entries=e; }
    if (!e->value && n) { e->value=malloc((size_t)n); if (e->value) WideCharToMultiByte(CP_UTF8,0,value,-1,e->value,n,NULL,NULL); }
    if (e->value) for (char *p=e->value;*p;++p) if (*p=='\\') *p='/';
    return e->value;
}
