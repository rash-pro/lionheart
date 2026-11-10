/* Apply fixture variables inside Windows because Wine replaces or removes
 * Unix environment variables before it starts a PE process. */
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
    int first=1;
    while (first<argc && wcscmp(argv[first],L"--")) {
        if (!wcscmp(argv[first],L"--set") && first+2<argc) {
            if (!SetEnvironmentVariableW(argv[first+1],argv[first+2])) return 2;
            first+=3;
        } else if (!wcscmp(argv[first],L"--unset") && first+1<argc) {
            if (!SetEnvironmentVariableW(argv[first+1],NULL)) return 2;
            first+=2;
        } else return 2;
    }
    if (++first>=argc) return 2;
    for (int i=first;i<argc;++i) size+=2*wcslen(argv[i])+4;
    command=calloc(size,sizeof(*command)); if (!command) return 2;
    p=command;
    for (int i=first;i<argc;++i) {
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
    startup.dwFlags=STARTF_USESTDHANDLES;
    startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (!CreateProcessW(NULL,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"test launcher CreateProcessW failed: %lu\n",(unsigned long)GetLastError());
        free(command); return 2;
    }
    WaitForSingleObject(process.hProcess,INFINITE); GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hProcess); CloseHandle(process.hThread); free(command); return (int)code;
}
