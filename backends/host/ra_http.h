#ifndef HOST_RA_HTTP_H
#define HOST_RA_HTTP_H
#include <stddef.h>
#include <stdint.h>
#include <SDL.h>

/* Worker-owned request and response. No guest or rc_client calls on the worker. */
typedef struct {
    const char *url, *post, *content_type, *user_agent;
    unsigned timeout_ms;
    SDL_atomic_t *cancel;
    char *body;
    size_t size;
    int status;
} host_ra_http_t;
int host_ra_http_init(void);
void host_ra_http_cleanup(void);
void host_ra_http_perform(host_ra_http_t *);
void host_ra_erase(void *, size_t);
#endif
