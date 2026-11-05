#include "identity.h"
#include "options.h"
#include <SDL.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FF8_PLAYER
#include "player_paths.h"
#endif

#define OPTION(key, values, field, kind) {key, values, kind, offsetof(host_options_t, field), HOST_VALUE_ENUM}
#define PRESENT(key, values, field, type) {key, values, HOST_OPTION_PRESENTATION, offsetof(host_options_t, field), type}
const host_option_t host_options[] = {
#if defined(FF8_PLAYER) || defined(FF8_GUI)
    {"disc1", "PATH", HOST_OPTION_LOGIC, offsetof(host_options_t, discs[0]), HOST_VALUE_MEDIA},
    {"disc2", "PATH", HOST_OPTION_LOGIC, offsetof(host_options_t, discs[1]), HOST_VALUE_MEDIA},
    {"disc3", "PATH", HOST_OPTION_LOGIC, offsetof(host_options_t, discs[2]), HOST_VALUE_MEDIA},
    {"disc4", "PATH", HOST_OPTION_LOGIC, offsetof(host_options_t, discs[3]), HOST_VALUE_MEDIA},
    {"card1", "PATH", HOST_OPTION_LOGIC, offsetof(host_options_t, cards[0]), HOST_VALUE_MEDIA},
    {"card2", "PATH", HOST_OPTION_LOGIC, offsetof(host_options_t, cards[1]), HOST_VALUE_MEDIA},
#endif
#ifdef FF8_PLAYER
    OPTION("verify-discs", "quick|full", verify_discs, HOST_OPTION_PRESENTATION),
#endif
    OPTION("pad", "digital|dualshock", dualshock, HOST_OPTION_LOGIC),
    OPTION("rumble", "0|1", rumble, HOST_OPTION_PRESENTATION),
    OPTION("fullscreen", "0|1", fullscreen, HOST_OPTION_PRESENTATION),
    OPTION("scale-mode", "aspect|integer|stretch|aspect-crop", scale_mode, HOST_OPTION_PRESENTATION),
    OPTION("filter", "nearest|linear", linear, HOST_OPTION_PRESENTATION),
    OPTION("pad-hotkey", "guide|off", pad_hotkey, HOST_OPTION_PRESENTATION),
    PRESENT("screenshot-dir", "PATH", screenshot_dir, HOST_VALUE_PATH),
    PRESENT("screenshot-at", "FRAME", screenshot_at, HOST_VALUE_FRAMES),
    OPTION("osd", "0|1", osd, HOST_OPTION_PRESENTATION),
    OPTION("ra", "0|1", ra, HOST_OPTION_PRESENTATION),
    OPTION("ra-notify", "0|1", ra_notify, HOST_OPTION_PRESENTATION),
    PRESENT("osd-test", "FRAME:LABEL[:VALUE]", osd_tests, HOST_VALUE_OSD_TEST),
    OPTION("crt", "off|pvm|tv|arcade", crt, HOST_OPTION_PRESENTATION),
    OPTION("renderer", "soft|vulkan", renderer, HOST_OPTION_PRESENTATION),
    PRESENT("internal-scale", "1..8", internal_scale, HOST_VALUE_NUMBER),
    OPTION("true-color", "0|1", true_color, HOST_OPTION_PRESENTATION),
    PRESENT("msaa", "1|2|4|8", msaa, HOST_VALUE_NUMBER),
    OPTION("pgxp", "off|vertex|perspective", pgxp, HOST_OPTION_PRESENTATION),
    PRESENT("movies", "PATH", movies, HOST_VALUE_PATH),
    PRESENT("movie-stats", "PATH", movie_stats, HOST_VALUE_PATH),
    PRESENT("dump-scaled-frames", "PATH", scaled_frames, HOST_VALUE_PATH),
    PRESENT("gpu-timings", "PATH", gpu_timings, HOST_VALUE_PATH),
    PRESENT("dump-presented-frames", "PATH", presented_frames, HOST_VALUE_PATH),
    PRESENT("present-size", "WxH", present_size, HOST_VALUE_SIZE),
    PRESENT("state-bridge", "PORT|fd:N", bridge, HOST_VALUE_BRIDGE),
#ifndef FF8_PLAYER
    PRESENT("achievements", "PATH", achievements, HOST_VALUE_PATH),
    PRESENT("achievement-progress", "PATH", progress, HOST_VALUE_PATH),
#endif
    PRESENT("savestate-dir", "PATH", state_dir, HOST_VALUE_PATH),
    PRESENT("load-state", "PATH", load_state, HOST_VALUE_PATH),
    PRESENT("save-state", "PATH", save_state, HOST_VALUE_PATH),
    PRESENT("save-at", "VBLANK", save_at, HOST_VALUE_NUMBER),
    OPTION("savestate-verify-disc", "quick|full", full_disc, HOST_OPTION_PRESENTATION),
    OPTION("savestate-cards", "live|snapshot", snapshot_cards, HOST_OPTION_LOGIC),
};
const size_t host_option_count = sizeof(host_options) / sizeof(host_options[0]);

void host_options_defaults(host_options_t *o) {
    memset(o, 0, sizeof(*o)); o->rumble = o->osd = o->ra_notify = 1;
    o->internal_scale = o->msaa = 1;
    o->present_size = 1920 | (1080ull << 32);
}
static const host_option_t *option(const char *key) {
    size_t i;
    for (i = 0; i < host_option_count; ++i) if (!strcmp(key, host_options[i].key)) return host_options + i;
    return NULL;
}
static int set(host_options_t *o, const host_option_t *opt, const char *value) {
    const char *p = opt->values;
    int index = 0;
    void *destination = (char *)o + opt->offset;
    if (opt->type == HOST_VALUE_PATH || opt->type == HOST_VALUE_MEDIA || opt->type == HOST_VALUE_BRIDGE) {
        if (strlen(value) >= 512) goto invalid;
        if (opt->type == HOST_VALUE_BRIDGE && *value) {
            const char *number = !strncmp(value,"fd:",3) ? value+3 : value;
            char *end; unsigned long port;
            if (!isdigit((unsigned char)*number)) goto invalid;
            errno = 0; port = strtoul(number,&end,10);
            if (*end || errno || port > (!strncmp(value,"fd:",3) ? 2147483647ul : 65535ul)) goto invalid;
        }
        strcpy(destination,value);
        if (!strcmp(opt->key,"disc1")) o->disc1_configured=1;
        return 1;
    }
    if (opt->type == HOST_VALUE_FRAMES) {
        char *end; size_t n=strlen(value), used=strlen(destination), count=0, i;
        if (!isdigit((unsigned char)*value)) goto invalid;
        errno=0; strtoull(value,&end,10);
        if (errno || *end) goto invalid;
        for (i=0;i<used;++i) count+=((char *)destination)[i]=='\n';
        if (count>=32 || used+n+2>4096) goto invalid;
        memcpy((char *)destination+used,value,n); ((char *)destination)[used+n]='\n';
        ((char *)destination)[used+n+1]=0; return 1;
    }
    if (opt->type == HOST_VALUE_OSD_TEST) {
        char *end; size_t n=strlen(value), used=strlen(destination), count=0, i;
        if (!isdigit((unsigned char)*value) || n>=512 || strchr(value,'\n') || strchr(value,'\r')) goto invalid;
        errno=0; strtoull(value,&end,10);
        if (errno || *end!=':' || !end[1] || end[1]==':') goto invalid;
        for (i=0;i<used;++i) count+=((char *)destination)[i]=='\n';
        if (count>=32 || used+n+2>4096) goto invalid;
        memcpy((char *)destination+used,value,n); ((char *)destination)[used+n]='\n';
        ((char *)destination)[used+n+1]=0; return 1;
    }
    if (opt->type == HOST_VALUE_SIZE) {
        char *end; unsigned long w, h;
        if (!isdigit((unsigned char)*value)) goto invalid;
        errno = 0; w = strtoul(value, &end, 10);
        if (errno || *end != 'x' || !isdigit((unsigned char)end[1])) goto invalid;
        h = strtoul(end + 1, &end, 10);
        if (errno || *end || w < 16 || h < 16 || w > 8192 || h > 8192) goto invalid;
        *(uint64_t *)destination = (uint64_t)w | ((uint64_t)h << 32); return 1;
    }
    if (opt->type == HOST_VALUE_NUMBER) {
        char *end; unsigned long long number;
        if (!isdigit((unsigned char)*value)) goto invalid;
        errno = 0; number = strtoull(value,&end,10);
        if (*end || errno) goto invalid;
        if (!strcmp(opt->key,"internal-scale") && (number < 1 || number > 8)) goto invalid;
        if (!strcmp(opt->key,"msaa") && number != 1 && number != 2 && number != 4 && number != 8) goto invalid;
        *(uint64_t *)destination = (uint64_t)number; return 1;
    }
    while (*p) {
        const char *end = strchr(p, '|');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (strlen(value) == n && !memcmp(value, p, n)) {
            *(int *)((char *)o + opt->offset) = index; return 1;
        }
        if (!end) break;
        p = end + 1; ++index;
    }
invalid:
    fprintf(stderr, "Invalid %s: %s (expected %s)\n", opt->key, value, opt->values);
    return 0;
}
int host_options_set(host_options_t *o, const char *key, const char *value) {
    const host_option_t *opt=option(key);
    if (!opt || !value || strchr(value,'\n') || strchr(value,'\r')) return 0;
    return set(o,opt,value);
}
static char *trim(char *p) {
    char *end;
    while (isspace((unsigned char)*p)) ++p;
    end = p + strlen(p);
    while (end > p && isspace((unsigned char)end[-1])) --end;
    *end = 0; return p;
}
static int load(host_options_t *o, const char *path, int explicit_path) {
    FILE *f = fopen(path, "r");
#ifdef FF8_PLAYER
    /* A validated 511-byte path plus its key and a comment must round-trip. */
    char buffer[4096];
#else
    char buffer[512];
#endif
    unsigned line = 0;
    int ok = 1;
    if (!f) {
        if (!explicit_path && errno == ENOENT) return 1;
        fprintf(stderr, "Config %s: %s\n", path, strerror(errno)); return 0;
    }
    while (fgets(buffer, sizeof(buffer), f)) {
        char *key, *equal, *comment;
        const host_option_t *opt;
        ++line;
        if (!strchr(buffer, '\n') && !feof(f)) { ok = 0; break; }
#ifdef FF8_PLAYER
        /* Embedded punctuation is path data. A marker followed immediately by
         * a filename character (FF8 #1.cue) is data in media values too. */
        equal = strchr(buffer, '=');
        int path_value=0;
        if (equal) {
            char name[128], *begin=buffer, *end=equal;
            while (isspace((unsigned char)*begin)) ++begin;
            while (end>begin && isspace((unsigned char)end[-1])) --end;
            if ((size_t)(end-begin)<sizeof(name)) {
                memcpy(name,begin,(size_t)(end-begin)); name[end-begin]=0;
                const host_option_t *candidate=option(name);
                path_value=candidate && (candidate->type==HOST_VALUE_MEDIA || candidate->type==HOST_VALUE_PATH);
            }
        }
        for (comment=buffer; *comment; ++comment) if ((*comment=='#' || *comment==';') &&
            (comment==buffer || isspace((unsigned char)comment[-1]))) {
            if (path_value && equal && comment>equal && comment[1] && !isspace((unsigned char)comment[1])) continue;
            *comment=0; break;
        }
#else
        comment = strpbrk(buffer, "#;"); if (comment) *comment = 0;
#endif
        key = trim(buffer); if (!*key) continue;
        equal = strchr(key, '=');
        if (!equal) { ok = 0; break; }
        *equal++ = 0; key = trim(key);
        if (!*key) { ok = 0; break; }
        opt = option(key);
        if (!opt) { fprintf(stderr, "Config %s:%u: unknown key '%s' (ignored)\n", path, line, key); continue; }
        if (!set(o, opt, trim(equal))) { ok = 0; break; }
#ifdef FF8_PLAYER
        if (opt->type == HOST_VALUE_MEDIA) {
            char *value = (char *)o + opt->offset;
#ifdef _WIN32
            for (char *p=value;*p;++p) if (*p=='\\') *p='/';
#endif
            const char *slash = strrchr(path,'/');
            if (*value && *value != '/' && !(value[0] && value[1] == ':') && slash) {
                char resolved[512];
                int n = snprintf(resolved,sizeof(resolved),"%.*s/%s",(int)(slash-path),path,value);
                if (n < 0 || (size_t)n >= sizeof(resolved)) {
                    fprintf(stderr,"Config %s:%u: resolved %s path exceeds 511 bytes\n",path,line,key);
                    ok = 0; break;
                }
                strcpy(value,resolved);
            }
        }
#endif
    }
    if (ferror(f)) ok = 0;
    if (fclose(f)) ok = 0;
    if (!ok) fprintf(stderr, "Config %s:%u: invalid setting (expected key=value, maximum %u characters per line)\n",
                     path,line,(unsigned)sizeof(buffer)-2);
    return ok;
}
int host_options_load(host_options_t *o, int argc, char **argv) {
    const char *explicit_path = NULL, *xdg;
    char path[4096];
    char *base;
    int i, n;
    for (i =
#ifdef FF8_PLAYER
         1;
#else
         2;
#endif
         i < argc; ++i) if (!strcmp(argv[i], "--config")) {
        if (++i == argc || !*argv[i]) { fprintf(stderr, "--config needs a path\n"); return 0; }
        explicit_path = argv[i];
    }
    if (explicit_path) {
        if (strlen(explicit_path) >= sizeof(o->config_path)) { fprintf(stderr,"Config path exceeds 4095 bytes\n"); return 0; }
        strcpy(o->config_path,explicit_path);
        return load(o, explicit_path, 1);
    }
#ifdef FF8_PLAYER
    if (host_player_portable(path,sizeof(path))) {
        n = snprintf(o->config_path,sizeof(o->config_path),"%s" HOST_INI,path);
        if (n < 0 || (size_t)n >= sizeof(o->config_path)) { fprintf(stderr,"Config path exceeds 4095 bytes\n"); return 0; }
        return load(o,o->config_path,0);
    }
#endif
    base = SDL_GetBasePath();
    if (base) {
        FILE *f;
        n = snprintf(path, sizeof(path), "%s" HOST_INI, base); SDL_free(base);
#ifdef _WIN32
        for (char *p=path;*p;++p) if (*p=='\\') *p='/';
#endif
        if (n < 0 || (size_t)n >= sizeof(path)) { fprintf(stderr,"Config path exceeds 4095 bytes\n"); return 0; }
        strcpy(o->config_path,path);
        f = fopen(path, "r");
        if (f) { fclose(f); return load(o, path, 1); }
        if (errno != ENOENT) return load(o, path, 1);
    }
#if defined(_WIN32) && defined(FF8_PLAYER)
    xdg = getenv("APPDATA");
    if (!xdg || !*xdg) { o->config_path[0]=0; return 1; }
    n = snprintf(path,sizeof(path),"%s/Lionheart/" HOST_INI,xdg);
#else
    xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) n = snprintf(path, sizeof(path), "%s/" HOST_XDG "/" HOST_INI, xdg);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) return 1;
        n = snprintf(path, sizeof(path), "%s/.config/" HOST_XDG "/" HOST_INI, home);
    }
#endif
    if (n < 0 || (size_t)n >= sizeof(path)) { fprintf(stderr, "Config path exceeds 4095 bytes\n"); return 0; }
    strcpy(o->config_path,path);
    return load(o, path, 0);
}
int host_options_cli(host_options_t *o, int argc, char **argv, int *i) {
    const host_option_t *opt;
    const char *arg = argv[*i];
    if (!strcmp(arg,"--gui")) { o->gui=1; o->no_gui=0; return 1; }
    if (!strcmp(arg,"--no-gui")) { o->no_gui=1; o->gui=0; return 1; }
    if (!strcmp(arg,"--gui-test")) {
        if (*i+1>=argc || strlen(argv[*i+1])>=sizeof(o->gui_test)) return -1;
        strcpy(o->gui_test,argv[++*i]); return 1;
    }
    if (!strcmp(arg, "--config")) { if (*i + 1 >= argc) return -1; ++*i; return 1; }
    if (!strcmp(arg, "--windowed")) { o->fullscreen = 0; return 1; }
    if (!strcmp(arg, "--fullscreen") || !strcmp(arg, "--true-color")) {
        if (*i + 1 < argc && strncmp(argv[*i + 1], "--", 2))
            return set(o, option(arg+2), argv[++*i]) ? 1 : -1;
        return set(o, option(arg+2), "1") ? 1 : -1;
    }
    if (strncmp(arg, "--", 2) || !(opt = option(arg + 2))) return 0;
    if (*i + 1 >= argc) { fprintf(stderr, "%s needs %s\n", arg, opt->values); return -1; }
    return set(o, opt, argv[++*i]) ? 1 : -1;
}

/* The service client is casual only: save states and fast-forward stay enabled. */
int host_options_ra_strict(const host_options_t *o) { (void)o; return 0; }
int host_options_ra_validate(const host_options_t *o, int fast) { (void)o; (void)fast; return 1; }
