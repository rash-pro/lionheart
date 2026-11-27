#ifndef FF8_HOST_MOD_IMPORT_H
#define FF8_HOST_MOD_IMPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOST_MOD_MAX_CHOICES 16
#define HOST_MOD_MAX_OPTIONS 32
#define HOST_MOD_MAX_ENTRIES 64

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
    unsigned audio_written;
    unsigned warnings;
    unsigned failed;
    char stage[96];
    char error[256];
} host_mod_import_status_t;

typedef struct {
    int value;
    char name[96];
} host_mod_xml_choice_t;

typedef struct {
    char id[64];
    char name[96];
    char description[192];
    int type; /* 0 = bool, 1 = list */
    int default_value;
    int selected_value;
    unsigned choice_count;
    host_mod_xml_choice_t choices[HOST_MOD_MAX_CHOICES];
} host_mod_xml_option_t;

typedef struct {
    char mod_name[128];
    char author[96];
    char version[48];
    unsigned option_count;
    host_mod_xml_option_t options[HOST_MOD_MAX_OPTIONS];
} host_mod_xml_config_t;

typedef struct {
    char id[64];
    int value;
} host_mod_option_override_t;

typedef struct {
    char id[64];
    char name[128];
    char source[512];
    int enabled;
    unsigned textures_written;
    unsigned models_written;
    unsigned movies_written;
    unsigned audio_written;
    unsigned warnings;
    unsigned failed;
    char summary_log[1024];
} host_mod_entry_t;

/* Inspect mod.xml metadata and configurable options in an .iro/.iroj archive or folder.
 * Returns 1 if mod.xml was found and parsed, 0 otherwise. */
int host_mod_inspect_options(const char *mod_path, host_mod_xml_config_t *out_config);

/* Synchronous CLI/library entry points.
 * Indexes PS1 FF8DISC*.IMG from disc_paths[0..disc_count-1], scans mod_paths[0..mod_count-1]
 * (.iroj/.iro archives, folders of archives, or unpacked FFNx directories), and writes
 * Lionheart-native HD textures, glTF .glb + manifest.json HD models, optional FMV movies,
 * and optional audio pack assets.
 * Returns 1 on success, 0 on fatal error (details in out_status->error). */
int host_mod_import_run(const char *const *disc_paths, unsigned disc_count,
                        const char *const *mod_paths, unsigned mod_count,
                        const char *textures_out, const char *models_out,
                        const char *movies_out,
                        host_mod_import_status_t *out_status);

int host_mod_import_run_ex(const char *const *disc_paths, unsigned disc_count,
                           const char *const *mod_paths, unsigned mod_count,
                           const char *textures_out, const char *models_out,
                           const char *movies_out, const char *audio_out,
                           const char *registry_dir,
                           const host_mod_option_override_t *overrides, unsigned override_count,
                           host_mod_import_status_t *out_status);

/* Asynchronous GUI entry points (run on a background thread).
 * Returns 1 if started, 0 if already running or invalid arguments. */
int host_mod_import_start_async(const char *const *disc_paths, unsigned disc_count,
                                const char *mod_path,
                                const char *textures_out, const char *models_out,
                                const char *movies_out);

int host_mod_import_start_async_ex(const char *const *disc_paths, unsigned disc_count,
                                   const char *mod_path,
                                   const char *textures_out, const char *models_out,
                                   const char *movies_out, const char *audio_out,
                                   const char *registry_dir,
                                   const host_mod_option_override_t *overrides, unsigned override_count);

/* Poll current async import status (safe from GUI thread every frame). */
void host_mod_import_poll(host_mod_import_status_t *out_status);

/* Join background import thread cleanly on shutdown. */
void host_mod_import_shutdown(void);

/* Imported mod registry management (stored in <data_dir>/mods-registry.json). */
unsigned host_mod_registry_list(const char *data_dir, host_mod_entry_t *out, unsigned capacity);
int host_mod_registry_set_enabled(const char *data_dir, const char *mod_id, int enabled,
                                  const char *textures_dir, const char *models_dir,
                                  const char *movies_dir);
int host_mod_registry_remove(const char *data_dir, const char *mod_id,
                             const char *textures_dir, const char *models_dir,
                             const char *movies_dir);

#ifdef __cplusplus
}

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace HostHD {

/* Exposed for clean-room unit tests. */
struct ModXmlResult {
    bool has_folder_rules = false;
    std::vector<std::string> active_folders;
    std::vector<std::string> declared_folders;
};

bool parse_mod_xml_config(const std::string &xml_text, host_mod_xml_config_t &out);
ModXmlResult parse_and_eval_mod_xml(const std::string &xml_text,
                                    const std::vector<std::pair<std::string, int>> &overrides = {});
bool evaluate_active_when_expr(const std::string &expr,
                               const std::vector<std::pair<std::string, int>> &vars);

struct IrosEntry {
    std::string name;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
};

bool parse_iros_directory(const std::string &path, std::vector<IrosEntry> &out, std::string *error = nullptr);
bool convert_battle_dat_to_glb_file(const std::string &model_id,
                                    const std::vector<uint8_t> &dat_bytes,
                                    const std::string &glb_out_path,
                                    std::vector<int> *out_bone_remap = nullptr);

} // namespace HostHD
#endif

#endif
