#define _POSIX_C_SOURCE 200809L
#include "ra.h"
#include "ra_http.h"
#include "ra_store.h"
#include "achievements.h"
#include "player_paths.h"
#include "rc_client.h"
#include "rc_client_game.h"
#include "../runtime/rc_trigger.h"
#include "rc_api_request.h"
#include "rc_api_runtime.h"
#include "rc_api_common.h"
#include "rc_hash.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FF8_VERSION
#define FF8_VERSION "dev"
#endif
enum { JOB_API,JOB_HASH,JOB_BADGE };
typedef struct session {
    struct session *next;
    rc_client_t *client;
    unsigned pending;
    int retired;
} session_t;
typedef struct job {
    struct job *next;
    unsigned kind,epoch,media,id;
    char *url,*post,*path;
    rc_client_server_callback_t callback;
    void *userdata;
    session_t *session;
    unsigned award_id;
    host_ra_http_t http;
    SDL_atomic_t cancel;
    uint8_t *badge;
    char hash[33];
    char agent[256];
} job_t;
typedef struct { unsigned id; uint8_t *badge; int requested,pending; uint64_t retry; char progress[32]; } badge_t;
static struct {
    host_ra_status_t status;
    rc_client_t *client;
    session_t *sessions;
    rc_client_async_handle_t *load;
    const cpu_t *cpu;
    SDL_Thread *worker; SDL_mutex *mutex; SDL_cond *wake;
    job_t *queue,*done,*active;
    int stop,ack,enabled,notify,blocked,token_login,network_error,configured;
    unsigned epoch,media,timeout;
    uint64_t retry_at;
    char directory[4096],credentials[4096],disc[4096],agent[256],test_host[128],token[256];
    badge_t *badges;
    unsigned count,*toasts,toast_count,toast_at;
    double toast_started;
} ra;

static void begin_load(void);
static void login_token(void);
static void collect_sessions(void) {
    session_t **at=&ra.sessions;
    while (*at) {
        session_t *s=*at;
        if (s->retired && !s->pending) { *at=s->next; rc_client_destroy(s->client); free(s); }
        else at=&s->next;
    }
}
static void cancelled(job_t *j) {
    static const char body[]="{\"Success\":false,\"Error\":\"Cancelled\"}";
    rc_api_server_response_t response={body,sizeof(body)-1,RC_API_SERVER_RESPONSE_CLIENT_ERROR};
    j->callback(&response,j->userdata); --j->session->pending;
}
static void free_job(job_t *j) {
    if (j->post) host_ra_erase(j->post,strlen(j->post));
    if (j->http.body) host_ra_erase(j->http.body,j->http.size);
    free(j->http.body); free(j->post); free(j->url); free(j->path); free(j->badge); free(j);
}
static void free_set(void) {
    for (unsigned i=0;i<ra.count;++i) free(ra.badges[i].badge);
    free(ra.badges); ra.badges=NULL; ra.count=0;
    free(ra.toasts); ra.toasts=NULL; ra.toast_count=ra.toast_at=0; ra.toast_started=-1;
    ra.status.total=ra.status.unlocked=ra.status.points=ra.status.earned=0;
    ra.status.title[0]=0; ra.status.downloaded=0; ++ra.status.badge_revision;
}
static int worker(void *unused) {
    (void)unused;
    SDL_LockMutex(ra.mutex);
    for (;;) {
        while (!ra.stop && (!ra.queue || ra.ack)) SDL_CondWait(ra.wake,ra.mutex);
        if (ra.stop) break;
        job_t *j=ra.queue; ra.queue=j->next; j->next=NULL; ra.active=j;
        SDL_UnlockMutex(ra.mutex);
        if (!SDL_AtomicGet(&j->cancel)) {
            if (j->kind==JOB_HASH) {
                rc_hash_iterator_t it; rc_hash_initialize_iterator(&it,j->path,NULL,0);
                if (!rc_hash_generate(j->hash,RC_CONSOLE_PLAYSTATION,&it)) j->hash[0]=0;
                rc_hash_destroy_iterator(&it);
            } else {
                if (j->kind==JOB_BADGE) {
                    size_t size=0; char *bytes=host_ra_file_read(j->path,&size,1024*1024,0);
                    if (bytes) { j->badge=host_ra_badge_decode(bytes,size); free(bytes); }
                }
                if (!j->badge) {
                    host_ra_http_perform(&j->http);
                    if (j->kind==JOB_BADGE && j->http.status==200 && !SDL_AtomicGet(&j->cancel)) {
                        j->badge=host_ra_badge_decode(j->http.body,j->http.size);
                        if (j->badge) host_ra_file_write(j->path,j->http.body,j->http.size);
                    }
                }
            }
        }
        SDL_LockMutex(ra.mutex);
        ra.active=NULL; ra.done=j; ra.ack=1;
    }
    SDL_UnlockMutex(ra.mutex); return 0;
}
static void enqueue(job_t *j) {
    j->epoch=ra.epoch; j->media=ra.media;
    j->http.url=j->url; j->http.post=j->post;
    snprintf(j->agent,sizeof(j->agent),"%s",ra.agent);
    j->http.user_agent=j->agent; j->http.timeout_ms=ra.timeout; j->http.cancel=&j->cancel;
    SDL_LockMutex(ra.mutex);
    job_t **tail=&ra.queue;
    /* API calls take priority over cosmetic downloads. */
    while (*tail && (j->kind==JOB_BADGE || (*tail)->kind!=JOB_BADGE)) tail=&(*tail)->next;
    j->next=*tail; *tail=j; SDL_CondSignal(ra.wake); SDL_UnlockMutex(ra.mutex);
}
static void cancel_session(void) {
    ++ra.epoch; ra.load=NULL;
    if (ra.client) {
        session_t *s=rc_client_get_userdata(ra.client);
        s->retired=1; ra.client=NULL;
    }
    if (ra.mutex) {
        SDL_LockMutex(ra.mutex);
        for (job_t *j=ra.queue;j;j=j->next) SDL_AtomicSet(&j->cancel,1);
        if (ra.active) SDL_AtomicSet(&ra.active->cancel,1);
        SDL_UnlockMutex(ra.mutex);
    }
    free_set(); ra.status.loading=0; ra.retry_at=0;
    collect_sessions();
}
static void reject(void) {
    ra.blocked=1; ra.status.state=HOST_RA_REJECTED; ra.status.error_id="ra.rejected.sub";
    host_ra_erase(ra.token,sizeof(ra.token));
    if (*ra.credentials && unlink(ra.credentials) && errno!=ENOENT) ra.status.storage_failed=1;
}
static int diagnostic_host(const char *value) {
    if (strncmp(value,"http://127.0.0.1:",17)) return 0;
    char *end; unsigned long port=strtoul(value+17,&end,10);
    return end!=value+17 && !*end && port>0 && port<=65535;
}
static char *request_url(const char *url) {
    if (*ra.test_host) {
        const char *scheme=strstr(url,"://"),*path=scheme ? strchr(scheme+3,'/') : NULL;
        size_t size=strlen(ra.test_host)+(path ? strlen(path) : 1)+1;
        char *result=malloc(size);
        if (result) snprintf(result,size,"%s%s",ra.test_host,path ? path : "/");
        return result;
    }
    /* Credentials only travel over TLS; badge URLs use the same rule. */
    return !strncmp(url,"https://",8) ? strdup(url) : NULL;
}
static void server_call(const rc_api_request_t *request,rc_client_server_callback_t cb,void *data,rc_client_t *client) {
    session_t *session=rc_client_get_userdata(client);
    job_t *j=calloc(1,sizeof(*j));
    if (j) {
        j->kind=JOB_API; j->callback=cb; j->userdata=data; j->session=session;
        j->url=request_url(request->url); j->post=request->post_data ? strdup(request->post_data) : NULL;
        if (j->post && !strncmp(j->post,"r=awardachievement&",19)) {
            const char *id=strstr(j->post,"&a=");
            if (id) j->award_id=(unsigned)strtoul(id+3,NULL,10);
        }
    }
    if (!j || !j->url || (request->post_data && !j->post) || !ra.enabled || ra.blocked || session->retired) {
        static const char body[]="{\"Success\":false,\"Error\":\"Request unavailable\"}";
        rc_api_server_response_t response={body,sizeof(body)-1,RC_API_SERVER_RESPONSE_CLIENT_ERROR};
        if (j) free_job(j);
        cb(&response,data); return;
    }
    ++session->pending; enqueue(j);
}
static uint32_t read_memory(uint32_t address,uint8_t *buffer,uint32_t size,rc_client_t *client) {
    (void)client;
    return ra.cpu ? host_achievements_read(address,buffer,size,(void *)ra.cpu) : 0;
}
static rc_clock_t clock_ms(const rc_client_t *client) { (void)client; return SDL_GetTicks64(); }
static void event(const rc_client_event_t *e,rc_client_t *client) {
    if (client!=ra.client) return;
    if (e->type==RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED) {
        for (unsigned i=0;i<ra.count;++i) if (ra.badges[i].id==e->achievement->id) ra.badges[i].pending=1;
        if (ra.notify) {
            unsigned *next=realloc(ra.toasts,(ra.toast_count+1)*sizeof(*next));
            if (next) { ra.toasts=next; next[ra.toast_count++]=e->achievement->id; }
        }
    } else if (e->type==RC_CLIENT_EVENT_DISCONNECTED) ra.status.state=HOST_RA_OFFLINE;
    else if (e->type==RC_CLIENT_EVENT_RECONNECTED && !ra.blocked) ra.status.state=HOST_RA_CONNECTED;
    else if (e->type==RC_CLIENT_EVENT_SERVER_ERROR) {
        /* An explicit server refusal is terminal, including an award refusal.
         * rc_client retains retryable transport failures itself. */
        reject();
    }
}
static int create_client(void) {
    if (ra.client) return 1;
    session_t *session=calloc(1,sizeof(*session));
    if (!session) return 0;
    ra.client=rc_client_create(read_memory,server_call);
    if (!ra.client) { free(session); return 0; }
    session->client=ra.client; session->next=ra.sessions; ra.sessions=session;
    rc_client_set_userdata(ra.client,session);
    rc_client_set_hardcore_enabled(ra.client,0);
    rc_client_set_allow_background_memory_reads(ra.client,0);
    rc_client_set_event_handler(ra.client,event);
    rc_client_set_get_time_millisecs_function(ra.client,clock_ms);
    if (*ra.test_host) rc_client_set_host(ra.client,ra.test_host);
    char clause[128]; rc_client_get_user_agent_clause(ra.client,clause,sizeof(clause));
    snprintf(ra.agent,sizeof(ra.agent),"Lionheart/%s (%s) %s",FF8_VERSION,
#ifdef _WIN32
        "Windows",
#else
        "Linux",
#endif
        clause);
    return 1;
}
static void login_done(int result,const char *error,rc_client_t *client,void *unused) {
    (void)error; (void)unused;
    if (ra.blocked || client!=ra.client) return;
    if (result==RC_OK) {
        const rc_client_user_t *user=rc_client_get_user_info(client);
        snprintf(ra.status.username,sizeof(ra.status.username),"%s",user->username);
        snprintf(ra.token,sizeof(ra.token),"%s",user->token);
        ra.status.storage_failed=!host_player_directory(ra.directory) || !host_ra_credentials_write(ra.credentials,user->username,user->token);
        ra.status.state=HOST_RA_CONNECTED; ra.status.error_id=NULL; ra.retry_at=0;
        begin_load();
    } else if (result==RC_ACCESS_DENIED || result==RC_EXPIRED_TOKEN || (ra.token_login && !ra.network_error)) reject();
    else if (ra.network_error) {
        ra.status.state=HOST_RA_OFFLINE; ra.status.error_id="ra.err.network";
        if (ra.token_login) ra.retry_at=SDL_GetTicks64()+60000;
    } else { ra.status.state=HOST_RA_SIGNED_OUT; ra.status.error_id="ra.err.credentials"; }
}
static void login_token(void) {
    if (!ra.enabled || ra.blocked || !*ra.token || !create_client()) return;
    ra.token_login=1; ra.network_error=0; ra.status.state=HOST_RA_SIGNING_IN;
    rc_client_begin_login_with_token(ra.client,ra.status.username,ra.token,login_done,NULL);
}
void host_ra_login(const char *user,const char *password) {
    if (!ra.enabled || !ra.worker || !user || !*user || !password || !*password) return;
    cancel_session(); ra.blocked=0; ra.token_login=0; ra.network_error=0;
    ra.status.error_id=NULL; ra.status.load_failed=ra.status.unknown_game=0;
    if (!create_client()) { ra.status.state=HOST_RA_UNAVAILABLE; return; }
    ra.status.state=HOST_RA_SIGNING_IN;
    rc_client_begin_login_with_password(ra.client,user,password,login_done,NULL);
}
static void load_done(int result,const char *error,rc_client_t *client,void *unused) {
    (void)error;
    if (client!=ra.client || (unsigned)(uintptr_t)unused!=ra.media) return;
    ra.load=NULL; ra.status.loading=0;
    if (ra.blocked) return;
    if (result==RC_ACCESS_DENIED || result==RC_EXPIRED_TOKEN || result==RC_INVALID_CREDENTIALS) { reject(); return; }
    const rc_client_game_t *game=rc_client_get_game_info(client);
    ra.status.unknown_game=result==RC_NO_GAME_LOADED || result==RC_NOT_FOUND || (result==RC_OK && (!game || !game->id));
    ra.status.load_failed=result!=RC_OK && !ra.status.unknown_game;
    if (ra.status.load_failed) {
        ra.status.state=ra.network_error ? HOST_RA_OFFLINE : HOST_RA_CONNECTED;
        ra.status.error_id=ra.network_error ? "ra.err.network" : "ach.load.title";
        ra.retry_at=SDL_GetTicks64()+60000; return;
    }
    if (ra.status.unknown_game) { free_set(); return; }
    snprintf(ra.status.title,sizeof(ra.status.title),"%s",game->title);
    ra.status.downloaded=time(NULL); ra.status.state=HOST_RA_CONNECTED; ra.status.error_id=NULL;
    rc_client_achievement_list_t *list=rc_client_create_achievement_list(client,RC_CLIENT_ACHIEVEMENT_CATEGORY_PROMOTED,RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
    if (!list) return;
    unsigned count=0;
    for (unsigned i=0;i<list->num_buckets;++i) count+=list->buckets[i].num_achievements;
    badge_t *next=calloc(count ? count : 1,sizeof(*next));
    if (next) {
        unsigned at=0;
        for (unsigned i=0;i<list->num_buckets;++i) for (unsigned j=0;j<list->buckets[i].num_achievements;++j) {
            next[at].id=list->buckets[i].achievements[j]->id;
            for (unsigned old=0;old<ra.count;++old) if (ra.badges[old].id==next[at].id) {
                next[at]=ra.badges[old]; next[at].requested=0; ra.badges[old].badge=NULL; break;
            }
            ++at;
        }
        for (unsigned i=0;i<ra.count;++i) free(ra.badges[i].badge);
        free(ra.badges); ra.badges=next; ra.count=count; ++ra.status.badge_revision;
    }
    rc_client_destroy_achievement_list(list); ra.status.total=ra.count;
}
static void begin_load(void) {
    if (!ra.client || !rc_client_get_user_info(ra.client) || !*ra.disc || ra.blocked) return;
    if (ra.load) { rc_client_abort_async(ra.client,ra.load); ra.load=NULL; }
    ra.status.loading=1; ra.status.load_failed=ra.status.unknown_game=0; ra.retry_at=0;
    job_t *j=calloc(1,sizeof(*j));
    if (!j || !(j->path=strdup(ra.disc))) { free(j); ra.status.loading=0; ra.status.load_failed=1; return; }
    j->kind=JOB_HASH; enqueue(j);
}
void host_ra_disc(const char *path) {
    if (!path || !strcmp(path,ra.disc)) return;
    if (strlen(path)>=sizeof(ra.disc)) { ra.status.load_failed=1; return; }
    strcpy(ra.disc,path); ++ra.media; begin_load();
}
void host_ra_retry(void) {
    if (!ra.enabled || ra.blocked || ra.status.loading || ra.status.state==HOST_RA_SIGNING_IN) return;
    ra.retry_at=0;
    if (ra.status.unknown_game) {
        cancel_session(); ra.status.unknown_game=0; login_token(); return;
    }
    if (ra.client && rc_client_get_user_info(ra.client)) begin_load();
    else login_token();
}
static int refusal(job_t *j) {
    if (j->http.status==403 || (j->http.status==401 && (!j->post || strncmp(j->post,"r=login",7) || ra.token_login))) return 1;
    rc_api_response_t result; rc_json_field_t fields[]={RC_JSON_NEW_FIELD("Success"),RC_JSON_NEW_FIELD("Error"),RC_JSON_NEW_FIELD("Code")};
    rc_api_server_response_t response={j->http.body ? j->http.body : "",j->http.size,j->http.status};
    memset(&result,0,sizeof(result));
    int code=rc_json_parse_server_response(&result,&response,fields,3);
    rc_buffer_destroy(&result.buffer);
    return code==RC_ACCESS_DENIED || code==RC_EXPIRED_TOKEN || (code==RC_INVALID_CREDENTIALS && (ra.token_login || ra.status.state!=HOST_RA_SIGNING_IN));
}
void host_ra_idle(void) {
    if (!ra.configured) return;
    SDL_LockMutex(ra.mutex); job_t *j=ra.done; ra.done=NULL; SDL_UnlockMutex(ra.mutex);
    if (j) {
        int current=j->epoch==ra.epoch;
        if (j->kind==JOB_API) {
            if (current) {
                ra.network_error=j->http.status<0 || j->http.status==429 || j->http.status>=500;
                if (refusal(j)) reject();
            }
            rc_api_server_response_t response={j->http.body ? j->http.body : "",j->http.size,
                j->http.status<0 ? RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR : j->http.status};
            if (!current) cancelled(j);
            else {
                j->callback(&response,j->userdata); --j->session->pending;
                if (j->award_id && !ra.network_error && !ra.blocked) {
                    rc_api_award_achievement_response_t award;
                    int result=rc_api_process_award_achievement_server_response(&award,&response);
                    if (result==RC_OK && award.response.succeeded && award.awarded_achievement_id==j->award_id)
                        for (unsigned i=0;i<ra.count;++i) if (ra.badges[i].id==j->award_id) ra.badges[i].pending=0;
                    rc_api_destroy_award_achievement_response(&award);
                }
            }
        } else if (current && j->media==ra.media && !ra.blocked) {
            if (j->kind==JOB_HASH && ra.client) {
                if (!*j->hash) { ra.status.loading=0; ra.status.load_failed=1; ra.status.error_id="ra.err.hash"; }
                else {
                    strcpy(ra.status.hash,j->hash);
                    ra.load=rc_client_get_game_info(ra.client) ? rc_client_begin_change_media(ra.client,j->hash,load_done,(void *)(uintptr_t)ra.media) :
                        rc_client_begin_load_game(ra.client,j->hash,load_done,(void *)(uintptr_t)ra.media);
                }
            } else if (j->kind==JOB_BADGE) for (unsigned i=0;i<ra.count;++i) if (ra.badges[i].id==j->id) {
                free(ra.badges[i].badge); ra.badges[i].badge=j->badge; j->badge=NULL;
                ra.badges[i].requested=0; ra.badges[i].retry=SDL_GetTicks64()+60000;
                ++ra.status.badge_revision; break;
            }
        }
        free_job(j);
        if (ra.blocked && ra.client) cancel_session();
        collect_sessions();
        SDL_LockMutex(ra.mutex); ra.ack=0; SDL_CondSignal(ra.wake); SDL_UnlockMutex(ra.mutex);
    }
    if (ra.client && !ra.blocked) rc_client_idle(ra.client);
    if (ra.blocked && ra.client) cancel_session();
    if (ra.retry_at && SDL_GetTicks64()>=ra.retry_at) host_ra_retry();
}
void host_ra_frame(const cpu_t *cpu) {
    ra.cpu=cpu;
    host_ra_idle();
    if (!ra.client || ra.blocked || ra.status.loading || ra.status.load_failed || ra.status.unknown_game) return;
    rc_client_do_frame(ra.client);
    if (ra.blocked) cancel_session();
}
void host_ra_reset(void) { if (ra.client) rc_client_reset(ra.client); }
void host_ra_notify(int enabled) {
    ra.notify=enabled;
    if (!enabled) { ra.toast_at=ra.toast_count=0; ra.toast_started=-1; }
}
void host_ra_signout(void) {
    cancel_session(); ra.enabled=0; ra.blocked=0; ra.status.state=HOST_RA_OFF;
    ra.status.error_id=NULL; ra.status.username[0]=0; ra.status.load_failed=ra.status.unknown_game=0;
    host_ra_erase(ra.token,sizeof(ra.token));
    if (*ra.credentials && unlink(ra.credentials) && errno!=ENOENT) ra.status.storage_failed=1;
}
void host_ra_enable(int enabled) {
    if (!enabled) { host_ra_signout(); return; }
    if (!ra.worker) return;
    ra.enabled=1;
    if (ra.status.state==HOST_RA_OFF) ra.status.state=HOST_RA_SIGNED_OUT;
}
void host_ra_configure(const char *data,int enabled,int notify) {
    host_ra_shutdown(); memset(&ra,0,sizeof(ra)); ra.toast_started=-1;
    ra.status.state=HOST_RA_OFF; ra.notify=notify; ra.timeout=5000;
    const char *test=getenv("LIONHEART_RA_TEST_HOST");
    if (test && (!diagnostic_host(test) || strlen(test)>=sizeof(ra.test_host))) { ra.status.state=HOST_RA_UNAVAILABLE; return; }
    if (test) {
        strcpy(ra.test_host,test);
        const char *timeout=getenv("LIONHEART_RA_TEST_TIMEOUT_MS");
        if (timeout) { char *end; unsigned long n=strtoul(timeout,&end,10); if (!*end && n>=50 && n<=5000) ra.timeout=(unsigned)n; }
    }
#ifdef _WIN32
    /* Tokens stay in the user's profile even when the player is portable. */
    char profile[4096]; const char *appdata=getenv("APPDATA");
    if (!appdata || snprintf(profile,sizeof(profile),"%s/Lionheart",appdata)>=(int)sizeof(profile)) { ra.status.state=HOST_RA_UNAVAILABLE; return; }
    data=profile;
#endif
    if (!data || !*data || snprintf(ra.directory,sizeof(ra.directory),"%s/ra",data)>=(int)sizeof(ra.directory) ||
        snprintf(ra.credentials,sizeof(ra.credentials),"%s/credentials",ra.directory)>=(int)sizeof(ra.credentials)) { ra.status.state=HOST_RA_UNAVAILABLE; return; }
    if (!host_ra_http_init()) { ra.status.state=HOST_RA_UNAVAILABLE; return; }
    ra.mutex=SDL_CreateMutex(); ra.wake=SDL_CreateCond();
    if (ra.mutex && ra.wake) ra.worker=SDL_CreateThread(worker,"achievements",NULL);
    if (!ra.worker) {
        SDL_DestroyCond(ra.wake); SDL_DestroyMutex(ra.mutex); ra.wake=NULL; ra.mutex=NULL;
        host_ra_http_cleanup(); ra.status.state=HOST_RA_UNAVAILABLE; return;
    }
    ra.configured=1; ra.enabled=enabled; ra.status.state=enabled ? HOST_RA_SIGNED_OUT : HOST_RA_OFF;
    if (enabled) {
        if (host_ra_credentials_read(ra.credentials,ra.status.username,sizeof(ra.status.username),ra.token,sizeof(ra.token))) login_token();
    }
}
void host_ra_shutdown(void) {
    if (!ra.configured) return;
    cancel_session();
    SDL_LockMutex(ra.mutex); ra.stop=1; ra.ack=0; SDL_CondSignal(ra.wake); SDL_UnlockMutex(ra.mutex);
    SDL_WaitThread(ra.worker,NULL);
    job_t *j=ra.done;
    if (j) { j->next=ra.queue; } else j=ra.queue;
    while (j) {
        job_t *next=j->next;
        if (j->callback) cancelled(j);
        free_job(j); j=next;
    }
    SDL_DestroyCond(ra.wake); SDL_DestroyMutex(ra.mutex); host_ra_http_cleanup();
    collect_sessions();
    host_ra_erase(&ra,sizeof(ra));
}
static void badge_request(unsigned index,const rc_client_achievement_t *a) {
    badge_t *b=ra.badges+index;
    if (b->badge || b->requested || SDL_GetTicks64()<b->retry || ra.blocked) return;
    char url[2048],directory[4096],path[4096];
    if (rc_client_achievement_get_image_url(a,RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED,url,sizeof(url))!=RC_OK) return;
    if (snprintf(directory,sizeof(directory),"%s/badges",ra.directory)>=(int)sizeof(directory) || !host_player_directory(directory) ||
        snprintf(path,sizeof(path),"%s/%u.png",directory,a->id)>=(int)sizeof(path)) return;
    job_t *j=calloc(1,sizeof(*j));
    if (!j) return;
    j->kind=JOB_BADGE; j->id=a->id; j->url=request_url(url); j->path=strdup(path);
    if (!j->url || !j->path) { free_job(j); return; }
    b->requested=1; enqueue(j);
}
int host_ra_achievement(unsigned index,host_ra_achievement_t *out) {
    if (!ra.client || index>=ra.count) return 0;
    const rc_client_achievement_t *a=rc_client_get_achievement_info(ra.client,ra.badges[index].id);
    if (!a) return 0;
    const char *progress=a->measured_progress;
    float percent=a->measured_percent;
    /* This pinned client's public formatter omits zero progress. Its public
     * record is the first member of the internal record; read only the target. */
    const rc_trigger_t *trigger=((const rc_client_achievement_info_t *)a)->trigger;
    if (!a->unlocked && trigger && trigger->measured_target) {
        if (!*progress) {
            percent=0;
            snprintf(ra.badges[index].progress,sizeof(ra.badges[index].progress),
                trigger->measured_as_percent ? "0%%" : "0 / %u",trigger->measured_target);
        } else {
            const char *slash=strchr(progress,'/');
            if (slash) snprintf(ra.badges[index].progress,sizeof(ra.badges[index].progress),"%.*s / %s",(int)(slash-progress),progress,slash+1);
            else snprintf(ra.badges[index].progress,sizeof(ra.badges[index].progress),"%s",progress);
        }
        progress=ra.badges[index].progress;
    }
    *out=(host_ra_achievement_t){a->id,a->points,a->title,a->description,progress,
        percent,a->unlock_time,!!a->unlocked,ra.badges[index].pending,ra.badges[index].badge};
    badge_request(index,a); return 1;
}
const host_ra_status_t *host_ra_status(void) {
    ra.status.total=ra.count; ra.status.unlocked=ra.status.points=ra.status.earned=0;
    for (unsigned i=0;ra.client && i<ra.count;++i) {
        const rc_client_achievement_t *a=rc_client_get_achievement_info(ra.client,ra.badges[i].id);
        if (a) { ra.status.points+=a->points; if (a->unlocked) { ++ra.status.unlocked; ra.status.earned+=a->points; } }
    }
    return &ra.status;
}
int host_ra_toast(host_ra_achievement_t *out,double now,float *opacity) {
    if (!ra.notify || ra.toast_at>=ra.toast_count) return 0;
    if (ra.toast_started<0 || now<ra.toast_started) ra.toast_started=now;
    double elapsed=now-ra.toast_started;
    if (elapsed>=2220) {
        ++ra.toast_at; ra.toast_started=-1;
        if (ra.toast_at==ra.toast_count) { ra.toast_at=ra.toast_count=0; return 0; }
        return host_ra_toast(out,now,opacity);
    }
    for (unsigned i=0;i<ra.count;++i) if (ra.badges[i].id==ra.toasts[ra.toast_at] && host_ra_achievement(i,out)) {
        *opacity=elapsed<120 ? (float)(elapsed/120) : elapsed<1920 ? 1 : (float)((2220-elapsed)/300);
        return 1;
    }
    return 0;
}
