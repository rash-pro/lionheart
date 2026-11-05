#ifndef HOST_RA_H
#define HOST_RA_H
#include <stddef.h>
#include <stdint.h>
#include <time.h>
struct cpu;
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    HOST_RA_OFF, HOST_RA_SIGNED_OUT, HOST_RA_SIGNING_IN, HOST_RA_CONNECTED,
    HOST_RA_OFFLINE, HOST_RA_REJECTED, HOST_RA_UNAVAILABLE
} host_ra_state_t;
typedef struct {
    host_ra_state_t state;
    const char *error_id;
    char username[128],title[256],hash[33];
    unsigned total,unlocked,points,earned,badge_revision;
    int loading,load_failed,unknown_game,storage_failed;
    time_t downloaded;
} host_ra_status_t;
typedef struct {
    unsigned id,points;
    const char *title,*description,*progress;
    float percent;
    time_t unlocked_at;
    int unlocked,pending;
    const uint8_t *badge;
} host_ra_achievement_t;
#ifdef FF8_RCHEEVOS
void host_ra_configure(const char *data_dir,int enabled,int notify);
void host_ra_enable(int);
void host_ra_notify(int);
void host_ra_login(const char *,const char *);
void host_ra_signout(void);
void host_ra_disc(const char *);
void host_ra_retry(void);
void host_ra_frame(const struct cpu *);
void host_ra_idle(void);
void host_ra_reset(void);
void host_ra_shutdown(void);
const host_ra_status_t *host_ra_status(void);
int host_ra_achievement(unsigned index,host_ra_achievement_t *);
int host_ra_toast(host_ra_achievement_t *,double now_ms,float *opacity);
#else
static inline void host_ra_configure(const char *d,int e,int n) { (void)d;(void)e;(void)n; }
static inline void host_ra_enable(int e) { (void)e; }
static inline void host_ra_notify(int n) { (void)n; }
static inline void host_ra_login(const char *u,const char *p) { (void)u;(void)p; }
static inline void host_ra_signout(void) {}
static inline void host_ra_disc(const char *p) { (void)p; }
static inline void host_ra_retry(void) {}
static inline void host_ra_frame(const struct cpu *c) { (void)c; }
static inline void host_ra_idle(void) {}
static inline void host_ra_reset(void) {}
static inline void host_ra_shutdown(void) {}
static inline const host_ra_status_t *host_ra_status(void) {
    static host_ra_status_t unavailable; unavailable.state=HOST_RA_UNAVAILABLE; return &unavailable;
}
static inline int host_ra_achievement(unsigned i,host_ra_achievement_t *a) { (void)i;(void)a;return 0; }
static inline int host_ra_toast(host_ra_achievement_t *a,double n,float *o) { (void)a;(void)n;(void)o;return 0; }
#endif
#ifdef __cplusplus
}
#endif
#endif
