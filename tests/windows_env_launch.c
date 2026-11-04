/* Wine populates APPDATA from the registry. Override it inside Windows for
 * isolated tests, then run the unmodified GUI executable and relay its exit. */
#include <windows.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdio.h>
int wmain(int argc, wchar_t **argv) {
    size_t size=1;
    wchar_t *command, *p;
    STARTUPINFOW startup={0};
    PROCESS_INFORMATION process;
    DWORD code=2;
    if (argc<3 || !SetEnvironmentVariableW(L"APPDATA",argv[1])) return 2;
#ifdef FF8_GUI_OFFSCREEN
    /* Wine can discard Unix SDL variables before creating a native PE process.
     * Set them inside Windows for this private GUI diagnostic launcher only. */
    if (!SetEnvironmentVariableW(L"SDL_VIDEODRIVER",L"dummy") ||
        !SetEnvironmentVariableW(L"SDL_AUDIODRIVER",L"dummy") ||
        !SetEnvironmentVariableW(L"SDL_RENDER_DRIVER",L"software")) return 2;
#endif
    for (int i=2;i<argc;++i) size+=2*wcslen(argv[i])+4;
    command=calloc(size,sizeof(*command)); if (!command) return 2;
    p=command;
    for (int i=2;i<argc;++i) {
        unsigned slashes=0;
        *p++=L'"';
        for (const wchar_t *a=argv[i];;a++) {
            if (*a==L'\\') { slashes++; continue; }
            if (!*a || *a==L'"') slashes*=2;
            while (slashes) { *p++=L'\\'; slashes--; }
            if (!*a) break;
            if (*a==L'"') *p++=L'\\';
            *p++=*a;
        }
        *p++=L'"'; *p++=L' ';
    }
    *p=0; startup.cb=sizeof(startup);
#ifdef NO_STDIO_LAUNCH
    startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=NULL; startup.hStdOutput=NULL; startup.hStdError=NULL;
    BOOL inherit=FALSE;
#else
    BOOL inherit=TRUE;
#endif
    if (!CreateProcessW(NULL,command,NULL,NULL,inherit,CREATE_NO_WINDOW,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"test launcher CreateProcessW failed: %lu\n",(unsigned long)GetLastError());
        free(command); return 2;
    }
    WaitForSingleObject(process.hProcess,INFINITE); GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hProcess); CloseHandle(process.hThread); free(command); return (int)code;
}
