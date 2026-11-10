#include "ra_http.h"
#include <stdlib.h>
#include <string.h>
int lh_http_get(const char *url) {
    SDL_atomic_t cancel = {0};
    host_ra_http_t request = {0};
    request.url = url; request.user_agent = "Lionheart native tests";
    request.timeout_ms = 3000; request.cancel = &cancel;
    if (!host_ra_http_init()) return 0;
    host_ra_http_perform(&request);
    int ok = request.status == 200 && request.size == 2 && !memcmp(request.body,"ok",2);
    free(request.body); host_ra_http_cleanup();
    return ok;
}
