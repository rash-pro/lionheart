#define _POSIX_C_SOURCE 200809L
#include "achievements.h"
#include "json_text.h"
#include "rc_runtime.h"
#include "rc_hash.h"
#include "rc_api_common.h"
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#endif

struct host_achievements {
    rc_runtime_t runtime;
    rc_api_response_t json;
    uint32_t *ids;
    unsigned char *unlocked;
    uint32_t count;
    char *progress;
    char patch_md5[33];
    char rich_presence[1024];
    int failed;
    host_achievement_event_t event;
    void *context;
};
/* rc_runtime's event callback has no userdata. Evaluation is synchronous and
 * confined to the host's single guest thread. */
static host_achievements_t *evaluating;

static char *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    char *data = NULL;
    long n;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > 16 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) goto done;
    data = malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); data = NULL; }
    if (data) { data[n] = 0; *size = (size_t)n; }
done:
    fclose(f);
    return data;
}

static int parse_json(rc_api_response_t *response, char *data, size_t size,
                      rc_json_field_t *fields, size_t count) {
    rc_api_server_response_t input;
    memset(&input, 0, sizeof(input));
    input.body = data; input.body_length = size; input.http_status_code = 200;
    return rc_json_parse_server_response(response, &input, fields, count) == RC_OK && response->succeeded;
}

static char *progress_directory(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    char *dir;
    if (!slash) return strdup(".");
    if (!n) n = 1;
    dir = malloc(n + 1);
    if (dir) { memcpy(dir, path, n); dir[n] = 0; }
    return dir;
}

/* Only this progress file's owned regular mkstemp files are candidates.
 * PID-tagged files from dead writers are stale immediately. Legacy random-only
 * names require 24 hours, so another running instance's fresh file is safe. */
static void clean_stale_progress(const char *path) {
    char *parent = progress_directory(path);
    const char *base = strrchr(path, '/');
    DIR *dir = parent ? opendir(parent) : NULL;
    struct dirent *entry;
    size_t prefix;
    time_t now = time(NULL);
    base = base ? base + 1 : path; prefix = strlen(base);
    if (!dir) { free(parent); return; }
    while ((entry = readdir(dir))) {
        const char *suffix, *random;
        char *end;
        long pid;
        struct stat st;
        int stale = 0;
        size_t i;
        if (strncmp(entry->d_name, base, prefix) || strncmp(entry->d_name + prefix, ".tmp.", 5)) continue;
        suffix = entry->d_name + prefix + 5; random = suffix;
        errno = 0; pid = strtol(suffix, &end, 10);
        if (end > suffix && *end == '.' && !errno && pid > 0 && pid <= 0x7fffffffL) {
            random = end + 1;
#ifdef _WIN32
            HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,(DWORD)pid);
            DWORD code;
            stale=process ? (GetExitCodeProcess(process,&code) && code!=STILL_ACTIVE) : GetLastError()==ERROR_INVALID_PARAMETER;
            if (process) CloseHandle(process);
#else
            stale = kill((pid_t)pid, 0) < 0 && errno == ESRCH;
#endif
        }
        if (strlen(random) != 6) continue;
        for (i = 0; i < 6 && isalnum((unsigned char)random[i]); ++i) {}
#ifdef _WIN32
        char candidate[8192];
        snprintf(candidate,sizeof(candidate),"%s/%s",parent,entry->d_name);
        if (i!=6 || stat(candidate,&st) || !S_ISREG(st.st_mode)) continue;
#else
        if (i != 6 || fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            !S_ISREG(st.st_mode) || st.st_uid != geteuid()) continue;
#endif
        if (random == suffix && now >= st.st_mtime && now - st.st_mtime >= 86400) stale = 1;
#ifdef _WIN32
        if (stale && unlink(candidate))
#else
        if (stale && unlinkat(dirfd(dir), entry->d_name, 0))
#endif
            fprintf(stderr, "Warning: stale achievement temp cleanup failed: %s\n", entry->d_name);
    }
    closedir(dir); free(parent);
}

static int save_progress(host_achievements_t *a) {
    size_t n = strlen(a->progress) + 40;
    char *temp = malloc(n), *parent = progress_directory(a->progress);
    FILE *f = NULL;
    struct stat original;
#ifndef _WIN32
    mode_t mode = 0600;
#endif
    uint32_t i;
    int ok = 0, first = 1, fd = -1, directory = -1, renamed = 0, created = 0;
    if (!temp || !parent) goto done;
    if (!stat(a->progress, &original)) {
#ifndef _WIN32
        mode = original.st_mode & 07777;
#endif
    }
    else if (errno != ENOENT) goto done;
#ifndef _WIN32
    directory = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) goto done;
#endif
    snprintf(temp, n, "%s.tmp.%ld.XXXXXX", a->progress, (long)getpid());
#ifdef _WIN32
    /* _mktemp_s chooses a name; O_EXCL makes publication race-safe. */
    if (_mktemp_s(temp,n)) goto done;
    fd = open(temp,O_CREAT|O_EXCL|O_RDWR|O_BINARY,0600);
#else
    fd = mkstemp(temp);
#endif
    if (fd < 0) goto done;
    created = 1;
    f = fdopen(fd, "w");
    if (!f) { close(fd); fd = -1; goto done; }
    ok = fprintf(f, "{\"version\":1,\"patch_md5\":\"%s\",\"unlocked\":[", a->patch_md5) >= 0;
    for (i = 0; i < a->count; ++i) if (a->unlocked[i]) {
        if (fprintf(f, "%s%u", first ? "" : ",", a->ids[i]) < 0) ok = 0;
        first = 0;
    }
    if (fprintf(f, "]}\n") < 0 || fflush(f) ||
#ifdef _WIN32
        _commit(fd)
#else
        fchmod(fd, mode) || fsync(fd)
#endif
       ) ok = 0;
    if (fclose(f)) ok = 0;
    f = NULL;
    if (ok) {
        if (rename(temp, a->progress)) ok = 0;
        else {
            renamed = 1;
#ifndef _WIN32
            if (fsync(directory)) ok = 0;
#endif
        }
    }
done:
    if (f) fclose(f);
    if (temp && created && !renamed) unlink(temp);
    if (directory >= 0) close(directory);
    free(temp); free(parent);
    return ok;
}

static int load_progress(host_achievements_t *a) {
    rc_json_field_t fields[] = {RC_JSON_NEW_FIELD("Success"), RC_JSON_NEW_FIELD("Error"),
        RC_JSON_NEW_FIELD("patch_md5"), RC_JSON_NEW_FIELD("unlocked")};
    rc_api_response_t response;
    uint32_t *ids = NULL, count = 0, i, j;
    const char *hash;
    size_t size;
    char *data;
    int ok;
    memset(&response, 0, sizeof(response)); rc_buffer_init(&response.buffer);
    errno = 0;
    data = read_file(a->progress, &size);
    if (!data) { rc_buffer_destroy(&response.buffer); return errno == ENOENT; }
    ok = parse_json(&response, data, size, fields, 4) &&
        rc_json_get_string(&hash, &response.buffer, &fields[2], "patch_md5") && hash && !strcmp(hash, a->patch_md5) &&
        rc_json_get_required_unum_array(&ids, &count, &response, &fields[3], "unlocked") == RC_OK;
    if (ok) for (i = 0; i < count; ++i) for (j = 0; j < a->count; ++j)
        if (a->ids[j] == ids[i]) { a->unlocked[j] = 1; rc_runtime_deactivate_achievement(&a->runtime, ids[i]); break; }
    free(data); rc_buffer_destroy(&response.buffer);
    return ok;
}

uint32_t host_achievements_read(uint32_t address, uint8_t *buffer, uint32_t count, void *cpu) {
    const cpu_t *c = cpu;
    uint32_t i;
    for (i = 0; i < count; ++i) {
        uint64_t at = (uint64_t)address + i;
        if (at < RAM_SIZE) buffer[i] = c->ram[at];
        else if (at < RAM_SIZE + sizeof(c->scratch)) buffer[i] = c->scratch[at - RAM_SIZE];
        else break;
    }
    return i;
}

static void runtime_event(const rc_runtime_event_t *e) {
    host_achievements_t *a = evaluating;
    uint32_t i;
    char json[160];
    if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) {
        for (i = 0; i < a->count; ++i) if (a->ids[i] == e->id && !a->unlocked[i]) {
            a->unlocked[i] = 1;
            a->failed = !save_progress(a);
            if (a->failed) fprintf(stderr, "Achievement progress write failed: %s\n", a->progress);
            fprintf(stderr, "Achievement unlocked (local): %u\n", e->id);
            snprintf(json, sizeof(json), "{\"type\":\"achievement\",\"id\":%u,\"persisted\":%s}", e->id, a->failed ? "false" : "true");
            if (a->event) a->event(a->context, json);
            break;
        }
    } else if (e->type >= RC_RUNTIME_EVENT_LBOARD_STARTED && e->type <= RC_RUNTIME_EVENT_LBOARD_TRIGGERED) {
        snprintf(json, sizeof(json), "{\"type\":\"leaderboard\",\"id\":%u,\"event\":%u,\"value\":%d}", e->id, e->type, e->value);
        fprintf(stderr, "Leaderboard (local): %s\n", json);
        if (a->event) a->event(a->context, json);
    } else if (e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_DISABLED || e->type == RC_RUNTIME_EVENT_LBOARD_DISABLED) {
        fprintf(stderr, "Achievement runtime invalid address: id=%u\n", e->id);
    }
}

static int valid_address(uint32_t address) {
    return address < RAM_SIZE + 1024;
}

host_achievements_t *host_achievements_open(const char *path, const char *card,
    const char *progress, const char *disc, host_achievement_event_t event, void *context) {
    rc_json_field_t root[] = {RC_JSON_NEW_FIELD("Success"), RC_JSON_NEW_FIELD("Error"), RC_JSON_NEW_FIELD("PatchData")};
    rc_json_field_t patch[] = {RC_JSON_NEW_FIELD("Achievements"), RC_JSON_NEW_FIELD("Leaderboards"), RC_JSON_NEW_FIELD("RichPresencePatch")};
    host_achievements_t *a = calloc(1, sizeof(*a));
    size_t size;
    char *data = read_file(path, &size);
    uint8_t md5[16];
    uint32_t i, j;
    int status = RC_OK;
    if (!a || !data) { fprintf(stderr, "Cannot read achievement patch: %s\n", path); free(a); free(data); return NULL; }
    rc_runtime_init(&a->runtime); rc_buffer_init(&a->json.buffer);
    a->event = event; a->context = context;
    rc_runtime_checksum(data, md5);
    for (i = 0; i < 16; ++i) snprintf(a->patch_md5 + i * 2, 3, "%02x", md5[i]);
    if (!parse_json(&a->json, data, size, root, 3) ||
        !rc_json_get_required_object(patch, 3, &a->json, &root[2], "PatchData")) goto fail;
    for (j = 0; j < 2; ++j) if (patch[j].value_start) {
        rc_json_iterator_t it;
        rc_json_field_t array;
        uint32_t count;
        if (!rc_json_get_required_array(&count, &array, &a->json, &patch[j], patch[j].name)) goto fail;
        it.json = array.value_start; it.end = array.value_end;
        if (!j) {
            a->count = count; a->ids = calloc(count ? count : 1, sizeof(*a->ids));
            a->unlocked = calloc(count ? count : 1, 1);
            if (!a->ids || !a->unlocked) goto fail;
        }
        for (i = 0; i < count; ++i) {
            rc_json_field_t fields[] = {RC_JSON_NEW_FIELD("ID"), RC_JSON_NEW_FIELD("MemAddr"), RC_JSON_NEW_FIELD("Mem")};
            uint32_t id, k;
            const char *definition;
            if (!rc_json_get_array_entry_object(fields, 3, &it) ||
                !rc_json_get_required_unum(&id, &a->json, &fields[0], "ID") ||
                !rc_json_get_required_string(&definition, &a->json, &fields[j && fields[2].value_start ? 2 : 1],
                    j && fields[2].value_start ? "Mem" : "MemAddr") || !definition || !id) goto fail;
            if (!j) {
                for (k = 0; k < i; ++k) if (a->ids[k] == id) goto fail;
                a->ids[i] = id;
                status = rc_runtime_activate_achievement(&a->runtime, id, definition, NULL, 0);
            } else {
                if (rc_runtime_get_lboard(&a->runtime, id)) goto fail;
                status = rc_runtime_activate_lboard(&a->runtime, id, definition, NULL, 0);
            }
            if (status != RC_OK) goto fail;
        }
    }
    if (patch[2].value_start) {
        const char *script;
        if (!rc_json_get_string(&script, &a->json.buffer, &patch[2], "RichPresencePatch")) goto fail;
        if (script && *script && (status = rc_runtime_activate_richpresence(&a->runtime, script, NULL, 0)) != RC_OK) goto fail;
    }
    evaluating = a;
    rc_runtime_validate_addresses(&a->runtime, runtime_event, valid_address);
    evaluating = NULL;
    if (progress) a->progress = strdup(progress);
    else {
        const char *base = card ? card : path;
        size_t n = strlen(base) + 64;
        a->progress = malloc(n);
        if (a->progress) snprintf(a->progress, n, "%s.achievements-%s.json", base, a->patch_md5);
    }
    if (!a->progress || !load_progress(a)) {
        fprintf(stderr, "Cannot load achievement progress: %s\n", a->progress ? a->progress : "(allocation failed)"); goto fail;
    }
    clean_stale_progress(a->progress);
    if (!save_progress(a)) {
        fprintf(stderr, "Cannot load/write achievement progress: %s\n", a->progress ? a->progress : "(allocation failed)"); goto fail;
    }
    if (disc) {
        char hash[33];
        rc_hash_iterator_t iterator;
        rc_hash_initialize_iterator(&iterator, disc, NULL, 0);
        if (!rc_hash_generate(hash, RC_CONSOLE_PLAYSTATION, &iterator)) {
            rc_hash_destroy_iterator(&iterator); fprintf(stderr, "Cannot compute PlayStation RA hash: %s\n", disc); goto fail;
        }
        rc_hash_destroy_iterator(&iterator);
        fprintf(stderr, "RetroAchievements PlayStation hash: %s\n", hash);
    } else fprintf(stderr, "RetroAchievements hash unavailable: supply --disc CUE_OR_BIN\n");
    fprintf(stderr, "Local achievements: %u; progress: %s\n", a->count, a->progress);
    free(data);
    return a;
fail:
    fprintf(stderr, "Invalid achievement patch/progress: %s (%s)\n", path,
        a->json.error_message ? a->json.error_message : rc_error_str(status));
    free(data); host_achievements_close(a);
    return NULL;
}

void host_achievements_frame(host_achievements_t *a, const cpu_t *c) {
    char text[1024], quoted[6146], event[6200];
    evaluating = a;
    rc_runtime_do_frame(&a->runtime, runtime_event, host_achievements_read, (void *)c, NULL);
    if (a->runtime.richpresence) {
        rc_runtime_get_richpresence(&a->runtime, text, sizeof(text), host_achievements_read, (void *)c, NULL);
        host_utf8_truncate(text, sizeof(text));
        if (strcmp(text, a->rich_presence) && host_json_quote(quoted, sizeof(quoted), text)) {
            memcpy(a->rich_presence, text, strlen(text) + 1);
            snprintf(event, sizeof(event), "{\"type\":\"rich_presence\",\"text\":%s}", quoted);
            if (a->event) a->event(a->context, event);
        }
    }
    evaluating = NULL;
}

int host_achievements_close(host_achievements_t *a) {
    int ok;
    if (!a) return 1;
    ok = !a->failed;
    rc_runtime_destroy(&a->runtime); rc_buffer_destroy(&a->json.buffer);
    free(a->ids); free(a->unlocked); free(a->progress); free(a);
    return ok;
}
