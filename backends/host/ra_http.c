#define _POSIX_C_SOURCE 200809L
#include "ra_http.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

#define RESPONSE_LIMIT (8u * 1024u * 1024u)
void host_ra_erase(void *data, size_t length) {
    volatile unsigned char *p=data;
    while (length--) *p++=0;
}
static int append(host_ra_http_t *r, const void *data, size_t size) {
    if (size>RESPONSE_LIMIT-r->size || SDL_AtomicGet(r->cancel)) return 0;
    char *next=realloc(r->body,r->size+size+1);
    if (!next) return 0;
    r->body=next; memcpy(next+r->size,data,size); r->size+=size; next[r->size]=0;
    return 1;
}
#ifdef _WIN32
int host_ra_http_init(void) { return 1; }
void host_ra_http_cleanup(void) {}
static wchar_t *wide(const char *s) {
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,NULL,0);
    wchar_t *out=size ? malloc((size_t)size*sizeof(*out)) : NULL;
    if (out && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,out,size)) { free(out); return NULL; }
    return out;
}
void host_ra_http_perform(host_ra_http_t *r) {
    wchar_t *url=wide(r->url), *agent=wide(r->user_agent), *host=NULL, *path=NULL;
    HINTERNET session=NULL,connection=NULL,request=NULL;
    URL_COMPONENTS parts; memset(&parts,0,sizeof(parts)); parts.dwStructSize=sizeof(parts);
    parts.dwHostNameLength=parts.dwUrlPathLength=parts.dwExtraInfoLength=(DWORD)-1;
    r->status=-1;
    if (!url || !agent || !WinHttpCrackUrl(url,0,0,&parts) || SDL_AtomicGet(r->cancel)) goto done;
    if (parts.nScheme!=INTERNET_SCHEME_HTTPS && parts.nScheme!=INTERNET_SCHEME_HTTP) goto done;
    host=calloc((size_t)parts.dwHostNameLength+1,sizeof(*host));
    path=calloc((size_t)parts.dwUrlPathLength+parts.dwExtraInfoLength+2,sizeof(*path));
    if (!host || !path) goto done;
    memcpy(host,parts.lpszHostName,parts.dwHostNameLength*sizeof(*host));
    memcpy(path,parts.lpszUrlPath,parts.dwUrlPathLength*sizeof(*path));
    if (parts.dwExtraInfoLength) memcpy(path+parts.dwUrlPathLength,parts.lpszExtraInfo,parts.dwExtraInfoLength*sizeof(*path));
    if (!*path) *path=L'/';
    session=WinHttpOpen(agent,!strncmp(r->url,"http://127.0.0.1:",17) ? WINHTTP_ACCESS_TYPE_NO_PROXY :
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!session) goto done;
    WinHttpSetTimeouts(session,(int)r->timeout_ms,(int)r->timeout_ms,(int)r->timeout_ms,(int)r->timeout_ms);
    connection=WinHttpConnect(session,host,parts.nPort,0);
    if (!connection) goto done;
    request=WinHttpOpenRequest(connection,r->post ? L"POST" : L"GET",path,NULL,WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,parts.nScheme==INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request) goto done;
    DWORD disable=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES,code=0,length=sizeof(code);
    if (!WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof(disable))) goto done;
    const wchar_t *headers=r->post ? L"Content-Type: application/x-www-form-urlencoded\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    DWORD bytes=r->post ? (DWORD)strlen(r->post) : 0;
    if (!WinHttpSendRequest(request,headers,r->post ? (DWORD)-1 : 0,(void *)r->post,bytes,bytes,0) ||
        !WinHttpReceiveResponse(request,NULL) ||
        !WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&length,WINHTTP_NO_HEADER_INDEX)) goto done;
    uint64_t started=SDL_GetTicks64();
    for (;;) {
        char buffer[16384]; DWORD got=0;
        if (SDL_AtomicGet(r->cancel) || SDL_GetTicks64()-started>r->timeout_ms ||
            !WinHttpReadData(request,buffer,sizeof(buffer),&got)) goto done;
        if (!got) break;
        if (!append(r,buffer,got)) goto done;
    }
    r->status=(int)code;
done:
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    free(path); free(host); free(agent); free(url);
}
#else
int host_ra_http_init(void) { return curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK; }
void host_ra_http_cleanup(void) { curl_global_cleanup(); }
static size_t receive(void *data,size_t size,size_t count,void *context) {
    if (size && count>SIZE_MAX/size) return 0;
    return append(context,data,size*count) ? size*count : 0;
}
static int progress(void *context,curl_off_t a,curl_off_t b,curl_off_t c,curl_off_t d) {
    (void)a; (void)b; (void)c; (void)d;
    return SDL_AtomicGet(((host_ra_http_t *)context)->cancel);
}
void host_ra_http_perform(host_ra_http_t *r) {
    CURL *curl=curl_easy_init();
    r->status=-1;
    if (!curl) return;
    struct curl_slist *headers=NULL;
    curl_easy_setopt(curl,CURLOPT_URL,r->url);
    if (!strncmp(r->url,"http://127.0.0.1:",17)) curl_easy_setopt(curl,CURLOPT_PROXY,"");
    curl_easy_setopt(curl,CURLOPT_USERAGENT,r->user_agent);
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,(long)r->timeout_ms);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,(long)r->timeout_ms);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,receive);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,r);
    curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(curl,CURLOPT_XFERINFODATA,r);
    curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
    if (r->post) {
        headers=curl_slist_append(headers,"Content-Type: application/x-www-form-urlencoded");
        curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
        curl_easy_setopt(curl,CURLOPT_POSTFIELDS,r->post);
    }
    if (curl_easy_perform(curl)==CURLE_OK) {
        long status=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status); r->status=(int)status;
    }
    curl_slist_free_all(headers); curl_easy_cleanup(curl);
}
#endif
