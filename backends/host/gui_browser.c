#define _POSIX_C_SOURCE 200809L
#include "gui_internal.h"
#include "player_paths.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

static int info(const char *path,int *directory,uint64_t *bytes) {
#ifdef _WIN32
    struct _stat64 st;
    if (host_win_stat(path,&st)) return 0;
#else
    struct stat st;
    if (stat(path,&st)) return 0;
#endif
    *directory=S_ISDIR(st.st_mode); *bytes=(uint64_t)st.st_size; return 1;
}
int gui_browser_accept(const gui_browser_t *browser,const char *path) {
    int directory; uint64_t bytes; size_t n=strlen(path);
    if (!info(path,&directory,&bytes)) return 0;
    if (directory) return 1;
    if (browser->kind==GUI_PICK_MOVIES || browser->kind==GUI_PICK_SCREENSHOTS) return 0;
    if (browser->show_all) return 1;
    if (browser->kind==GUI_PICK_DISC) return n>=4 && (!strcasecmp(path+n-4,".cue") || !strcasecmp(path+n-4,".bin"));
    if (browser->kind==GUI_PICK_ARCHIVE) return n>=4 && !strcasecmp(path+n-4,".zip");
    return n>=4 && (!strcasecmp(path+n-4,".mcr") || !strcasecmp(path+n-4,".mcd") || !strcasecmp(path+n-4,".srm"));
}
static int compare(const void *a,const void *b) {
    const gui_browser_entry_t *left=a,*right=b;
    if (left->directory!=right->directory) return right->directory-left->directory;
    return strcasecmp(left->name,right->name);
}
static void add_entry(gui_browser_t *browser,const char *name) {
    gui_browser_entry_t *entry; int n;
    if (!strcmp(name,".") || !strcmp(name,"..") || browser->count>=GUI_BROWSER_MAX) return;
    entry=browser->entries+browser->count;
    n=snprintf(entry->path,sizeof(entry->path),"%s%s%s",browser->folder,browser->folder[strlen(browser->folder)-1]=='/' ? "" : "/",name);
    if (n<0 || (size_t)n>=sizeof(entry->path) || !gui_browser_accept(browser,entry->path) || !info(entry->path,&entry->directory,&entry->bytes)) return;
    snprintf(entry->name,sizeof(entry->name),"%s",name); ++browser->count;
}
int gui_browser_refresh(gui_browser_t *browser) {
    browser->count=0; browser->error[0]=0;
#ifdef _WIN32
    wchar_t wide[2048]; char search[1024]; WIN32_FIND_DATAW data;
    snprintf(search,sizeof(search),"%s/*",browser->folder);
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,search,-1,wide,2048)) goto failed;
    HANDLE find=FindFirstFileW(wide,&data);
    if (find==INVALID_HANDLE_VALUE) goto failed;
    do {
        char name[512];
        if (WideCharToMultiByte(CP_UTF8,0,data.cFileName,-1,name,sizeof(name),NULL,NULL)) add_entry(browser,name);
    } while (FindNextFileW(find,&data));
    FindClose(find);
#else
    DIR *directory=opendir(browser->folder); struct dirent *entry;
    if (!directory) goto failed;
    while ((entry=readdir(directory))) add_entry(browser,entry->d_name);
    closedir(directory);
#endif
    qsort(browser->entries,browser->count,sizeof(browser->entries[0]),compare);
    snprintf(browser->path,sizeof(browser->path),"%s",browser->folder); return 1;
failed:
    snprintf(browser->error,sizeof(browser->error),"%s",host_gui_string("browser.error")); return 0;
}
int gui_browser_open(gui_browser_t *browser,const char *folder,gui_pick_t kind) {
    int directory; uint64_t bytes;
    if (!folder || !*folder || strlen(folder)>=sizeof(browser->folder) || !info(folder,&directory,&bytes) || !directory) return 0;
    snprintf(browser->folder,sizeof(browser->folder),"%s",folder);
#ifdef _WIN32
    for (char *p=browser->folder;*p;++p) if (*p=='\\') *p='/';
#endif
    while (strlen(browser->folder)>1 && browser->folder[strlen(browser->folder)-1]=='/'
#ifdef _WIN32
           && strlen(browser->folder)>3
#endif
           ) browser->folder[strlen(browser->folder)-1]=0;
    browser->kind=kind; return gui_browser_refresh(browser);
}
int gui_browser_up(gui_browser_t *browser) {
    char parent[512],*slash;
    snprintf(parent,sizeof(parent),"%s",browser->folder); slash=strrchr(parent,'/');
    if (!slash) return 0;
    if (slash==parent) slash[1]=0;
#ifdef _WIN32
    else if (slash==parent+2 && parent[1]==':') slash[1]=0;
#endif
    else *slash=0;
    return gui_browser_open(browser,parent,browser->kind);
}
static void place(char names[][512],char paths[][512],unsigned *count,unsigned capacity,const char *name,const char *path) {
    int directory; uint64_t bytes;
    if (*count>=capacity || !path || !*path || strlen(path)>=512 || !info(path,&directory,&bytes) || !directory) return;
    for (unsigned i=0;i<*count;++i) if (!strcmp(paths[i],path)) return;
    snprintf(names[*count],512,"%s",name); snprintf(paths[*count],512,"%s",path); ++*count;
}
unsigned gui_browser_places(char names[][512],char paths[][512],unsigned capacity,const char *last) {
    unsigned count=0; const char *home=getenv(
#ifdef _WIN32
    "USERPROFILE"
#else
    "HOME"
#endif
    ); char downloads[512];
    place(names,paths,&count,capacity,host_gui_string("browser.home"),home);
    place(names,paths,&count,capacity,host_gui_string("browser.last"),last);
    if (home && snprintf(downloads,sizeof(downloads),"%s/Downloads",home)<(int)sizeof(downloads)) place(names,paths,&count,capacity,host_gui_string("browser.downloads"),downloads);
#ifdef _WIN32
    DWORD drives=GetLogicalDrives();
    for (unsigned i=0;i<26;++i) if (drives&(1u<<i)) {
        char drive[4]={(char)('A'+i),':','/',0}; place(names,paths,&count,capacity,drive,drive);
    }
#else
    place(names,paths,&count,capacity,host_gui_string("browser.root"),"/");
    const char *user=getenv("USER");
    if (user && !strchr(user,'/')) for (unsigned i=0;i<2;++i) {
        char mount[512]; DIR *directory; struct dirent *entry;
        snprintf(mount,sizeof(mount),i ? "/run/media/%s" : "/media/%s",user);
        directory=opendir(mount); if (!directory) continue;
        while ((entry=readdir(directory))) if (strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) {
            char path[1024];
            snprintf(path,sizeof(path),"%s/%s",mount,entry->d_name);
            place(names,paths,&count,capacity,entry->d_name,path);
        }
        closedir(directory);
    }
#endif
    return count;
}
int gui_state_load(const char *data_dir,char *folder,size_t capacity,int *input,int *native_offered) {
    char path[4096],line[1024]; FILE *file;
    if (!data_dir || !*data_dir || snprintf(path,sizeof(path),"%s/gui-state.ini",data_dir)>=(int)sizeof(path)) return 0;
    file=fopen(path,"r"); if (!file) return 0;
    while (fgets(line,sizeof(line),file)) {
        line[strcspn(line,"\r\n")]=0;
        if (!strncmp(line,"last-folder=",12) && strlen(line+12)<capacity) strcpy(folder,line+12);
        if (!strncmp(line,"last-input=",11)) {
            const char *value=line+11;
            if (!strcmp(value,"pad")) *input=HOST_GUI_PAD;
            else if (!strcmp(value,"mouse")) *input=HOST_GUI_MOUSE;
            else *input=HOST_GUI_KEYBOARD;
        }
        if (native_offered && !strcmp(line,"native-offered=1")) *native_offered=1;
    }
    return fclose(file)==0;
}
int gui_state_save(const char *data_dir,const char *folder,int input,int native_offered) {
    char path[4096],temporary[4096]; FILE *file; int ok,n;
    if (!data_dir || !*data_dir || strchr(folder,'\r') || strchr(folder,'\n') || !host_player_directory(data_dir)) return 0;
    n=snprintf(path,sizeof(path),"%s/gui-state.ini",data_dir);
    if (n<0 || (size_t)n>=sizeof(path)) return 0;
    n=snprintf(temporary,sizeof(temporary),"%s/gui-state.ini.tmp",data_dir);
    if (n<0 || (size_t)n>=sizeof(temporary)) return 0;
    file=fopen(temporary,"w"); if (!file) return 0;
    ok=fprintf(file,"last-folder=%s\nlast-input=%s\n%s",folder,input==HOST_GUI_PAD ? "pad" : input==HOST_GUI_MOUSE ? "mouse" : "keyboard",
               native_offered ? "native-offered=1\n" : "")>0;
    if (fclose(file)) ok=0;
    if (ok) ok=!rename(temporary,path);
    if (!ok) remove(temporary);
    return ok;
}
