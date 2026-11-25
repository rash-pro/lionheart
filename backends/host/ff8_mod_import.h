#ifndef FF8_HOST_MOD_IMPORT_H
#define FF8_HOST_MOD_IMPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int running;
    int finished;
    int ok;
    unsigned done;
    unsigned total;
    unsigned textures_written;
    unsigned textures_skipped;
    unsigned models_written;
    unsigned movies_written;
    unsigned failed;
    char stage[96];
    char error[256];
} host_mod_import_status_t;

/* Synchronous CLI/library entry point.
 * Indexes PS1 FF8DISC*.IMG from disc_paths[0..disc_count-1], scans mod_paths[0..mod_count-1]
 * (.iroj/.iro archives, folders of archives, or unpacked FFNx directories), and writes
 * Lionheart-native HD textures, glTF .glb + manifest.json HD models, and optional FMV movies.
 * Returns 1 on success, 0 on fatal error (details in out_status->error). */
int host_mod_import_run(const char *const *disc_paths, unsigned disc_count,
                        const char *const *mod_paths, unsigned mod_count,
                        const char *textures_out, const char *models_out,
                        const char *movies_out,
                        host_mod_import_status_t *out_status);

/* Asynchronous GUI entry point (runs on a background thread).
 * Returns 1 if started, 0 if already running or invalid arguments. */
int host_mod_import_start_async(const char *const *disc_paths, unsigned disc_count,
                                const char *mod_path,
                                const char *textures_out, const char *models_out,
                                const char *movies_out);

/* Poll current async import status (safe from GUI thread every frame). */
void host_mod_import_poll(host_mod_import_status_t *out_status);

/* Join background import thread cleanly on shutdown. */
void host_mod_import_shutdown(void);

#ifdef __cplusplus
}

#include <cstdint>
#include <string>
#include <vector>

namespace HostHD {

/* Exposed for clean-room unit tests. */
struct ModXmlResult {
    bool has_folder_rules = false;
    std::vector<std::string> active_folders;
    std::vector<std::string> declared_folders;
};

ModXmlResult parse_and_eval_mod_xml(const std::string &xml_text);
bool evaluate_active_when_expr(const std::string &expr,
                               const std::vector<std::pair<std::string, int>> &vars);

struct IrosEntry {
    std::string name;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
};

bool parse_iros_directory(const std::string &path, std::vector<IrosEntry> &out, std::string *error = nullptr);

} // namespace HostHD
#endif

#endif
