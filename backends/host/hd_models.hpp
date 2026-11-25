#ifndef FF8_HOST_HD_MODELS_HPP
#define FF8_HOST_HD_MODELS_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HostHD {

static constexpr size_t kMaxHdBones = 128;

enum class AlphaMode : uint8_t {
    Opaque = 0,
    Mask = 1,
    Blend = 2
};

struct ModelVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    uint16_t joints[4];
    float weights[4];
};

struct ModelTexture {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<uint8_t> rgba;
};

struct ModelPrimitive {
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    int texture_index = -1;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    float alpha_cutoff = 0.5f;
    float base_color_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    bool double_sided = false;
};

struct HdModel {
    std::string id;
    float scale = 1.0f;
    float offset[3] = {0.0f, 0.0f, 0.0f};
    float rotation_euler[3] = {0.0f, 0.0f, 0.0f}; /* degrees: X, Y, Z */
    bool flip_winding = false;
    bool double_sided = false;
    bool has_gltf_skin = false; /* true when GLB defines glTF skins/inverseBindMatrices */
    std::vector<int> bone_remap; /* optional glTF joint -> PS1 GTE bone run index */
    std::vector<uint64_t> vram_texture_hashes;
    std::vector<uint64_t> gte_run_keys;
    uint64_t generation = 0;
    std::vector<ModelPrimitive> primitives;
    std::vector<ModelTexture> textures;

    /* Builds the 3x4 local affine matrix (scale * Rz * Ry * Rx + offset). */
    void local_transform(float rows[3][4]) const;
};

/* PS1 GTE bone transform converted from fixed-point (1.12 rotation, integer translation). */
struct GteBoneMatrix {
    float r[3][3];
    float t[3];
};

struct ProjectedVertex {
    float x, y, sz;
    float u, v;
};

/* Clean-room glTF 2.0 (.glb) + manifest.json HD model pack registry.
 * Presentation-only: never alters guest RAM or software reference output. */
class ModelRegistry {
public:
    ModelRegistry() = default;

    void configure(bool enabled, const std::string &models_dir);
    void reload();
    bool enabled() const { return enabled_atomic_.load(std::memory_order_relaxed); }
    bool has_any_active() const { return active_atomic_.load(std::memory_order_relaxed); }
    uint64_t generation() const { return generation_atomic_.load(std::memory_order_relaxed); }
    const std::string &directory() const;
    size_t discovered_count() const;
    bool has_model(const std::string &id) const;
    bool has_explicit_match_keys(const std::string &id) const;

    /* Loads (and caches) an HD model by canonical FF8 ID, e.g. "field/d000" or "battle/c0m001". */
    std::shared_ptr<const HdModel> get(const std::string &id);

    /* Looks up a canonical ID by PS1 VRAM texture hash or GTE run key from manifest.json. */
    std::string find_by_texture_hash(uint64_t tex_hash) const;
    std::string find_by_run_key(uint64_t run_key) const;
    std::shared_ptr<const HdModel> get_by_texture_hash(uint64_t tex_hash);

    /* Parses a glTF 2.0 binary (.glb) file into an HdModel. */
    static bool load_glb(const std::string &path, HdModel &out, std::string *error = nullptr);

    /* Converts a packed PS1 GTE setup (control regs 0..7) into a float32 bone matrix. */
    static GteBoneMatrix from_gte_setup(const uint32_t rotation[5], const int32_t translation[3]);

    /* Evaluates skinned/rigid vertex positions in view space and projects them with
     * GTE perspective parameters (ofx, ofy, h). */
    static void evaluate_pose(const HdModel &model, const GteBoneMatrix *bones, size_t bone_count,
                              float ofx, float ofy, float h,
                              std::vector<std::vector<ProjectedVertex>> &out_primitives);

    /* Deterministic 64-bit FNV-1a VRAM hash (matches TextureProvider::hash_vram_rect). */
    static uint64_t hash_vram_rect(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *vram);

private:
    struct Entry {
        std::string path;
        float scale = 1.0f;
        float offset[3] = {0.0f, 0.0f, 0.0f};
        float rotation_euler[3] = {0.0f, 0.0f, 0.0f};
        bool flip_winding = false;
        bool double_sided = false;
        std::vector<int> bone_remap;
        std::vector<uint64_t> vram_texture_hashes;
        std::vector<uint64_t> gte_run_keys;
    };
    void scan_locked();

    mutable std::mutex mutex_;
    std::atomic<bool> enabled_atomic_{false};
    std::atomic<bool> active_atomic_{false};
    std::atomic<uint64_t> generation_atomic_{0};
    bool enabled_ = false;
    std::string dir_;
    uint64_t generation_ = 0;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<uint64_t, std::string> tex_hash_to_id_;
    std::unordered_map<uint64_t, std::string> run_key_to_id_;
    std::unordered_map<std::string, std::shared_ptr<const HdModel>> cache_;
};

/* Read-only FF8 scene metadata sampled from guest RAM (USA SLUS-00892). */
enum class SceneMode : uint16_t {
    Unknown = 0,
    Field = 1,
    World = 2,
    Battle = 3,
    Menu = 6
};

struct SceneContext {
    SceneMode mode = SceneMode::Unknown;
    uint16_t field_id = 0;
    uint8_t party[3] = {0, 0xff, 0xff};
};

/* Consecutive GTE RTPS/RTPT projections sharing one bone setup. */
struct GteTransformRun {
    uint32_t raw_rot[5] = {};
    int32_t raw_tr[3] = {};
    int32_t raw_ofx = 0;
    int32_t raw_ofy = 0;
    uint32_t raw_h = 0;
    GteBoneMatrix bone{};
    float ofx = 0.0f;
    float ofy = 0.0f;
    float h = 0.0f;
    uint32_t vertex_count = 0;
    uint64_t run_key = 0;
    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    float min_sz = 1e9f, max_sz = 0.0f, sum_sz = 0.0f;
    float avg_sz() const { return vertex_count ? (sum_sz / float(vertex_count)) : 1.0f; }
};

/* One matched HD 3D model draw command queued for the Vulkan 3D render pass. */
struct HdModelDrawCall {
    std::shared_ptr<const HdModel> model;
    std::string model_id;
    std::vector<GteBoneMatrix> bones;
    float ofx = 0.0f, ofy = 0.0f, h = 0.0f;
    float draw_offset_x = 0.0f, draw_offset_y = 0.0f;
    float tint_rgb[3] = {1.0f, 1.0f, 1.0f};
    float min_sz = 1e9f, max_sz = 0.0f, avg_sz = 0.0f;
    int clip_left = 0, clip_top = 0, clip_right = 1023, clip_bottom = 511;
    int bbox_left = 1024, bbox_top = 512, bbox_right = -1, bbox_bottom = -1;
    uint32_t suppressed_triangles = 0;
};

/* Triangle observation input from the Vulkan GPU wrapper (presentation-only). */
struct TriangleObservation {
    int x[3] = {};
    int y[3] = {};
    int r[3] = {128, 128, 128};
    int g[3] = {128, 128, 128};
    int b[3] = {128, 128, 128};
    int u[3] = {};
    int v[3] = {};
    bool textured = false;
    bool raw = false;
    bool semi = false;
    unsigned tex_page_x = 0;
    unsigned tex_page_y = 0;
    unsigned tex_depth = 0; /* 0=4bpp, 1=8bpp, 2/3=15bpp */
    int clip_left = 0, clip_top = 0, clip_right = 1023, clip_bottom = 511;
    int draw_offset_x = 0, draw_offset_y = 0;
    bool has_draw_offset = false;
    uint64_t explicit_tex_hash = 0; /* optional override for unit tests */
};

struct ObserveDecision {
    bool suppress_ps1_triangle = false;
    bool flush_pending_hd_models = false;
    uint32_t matched_cluster = UINT32_MAX;
    float triangle_sz = 0.0f;
};

/* Clean-room live GTE scene/bone observer, entity matcher, lighting tint
 * capturer, and Ordering-Table depth/mask flush coordinator.
 *
 * Thread-safety: record_gte_projection() may run on the guest CPU thread while
 * observe_triangle(), take_pending_draws(), and end_frame() run on the Vulkan
 * presenter thread (e.g. when Smooth Motion is enabled). */
class HdSceneTracker {
public:
    HdSceneTracker() = default;

    void reset();

    /* Track VRAM texture uploads for hash-based HD model matching. */
    void record_vram_upload(unsigned x, unsigned y, unsigned w, unsigned h, const uint16_t *vram);
    void clear_vram_region(unsigned x, unsigned y, unsigned w, unsigned h);
    uint64_t lookup_vram_hash(unsigned tex_page_x, unsigned tex_page_y, unsigned tex_depth,
                              int u0, int v0, int u1, int v1) const;

    /* Called from the GTE projection sink on every RTPS/RTPT (when active). */
    void record_gte_projection(const uint32_t rotation[5], const int32_t translation[3],
                               int32_t ofx_q16, int32_t ofy_q16, uint32_t h,
                               const int16_t input_v[3], uint32_t packed_sxy,
                               float sx_subpx, float sy_subpx, uint32_t sz,
                               const SceneContext &ctx);

    /* Observes a Vulkan PS1 triangle, captures live GTE lighting tint, decides
     * selective PS1 suppression, and signals when foreground masks/overlapping
     * geometry require flushing pending HD models first. */
    ObserveDecision observe_triangle(const TriangleObservation &tri, ModelRegistry &registry);

    /* Returns true if a non-replaced 2D sprite/rectangle (e.g. foreground field
     * mask, dialogue box, HUD) overlaps any pending HD model draw call. */
    bool should_flush_for_rect(int x, int y, int w, int h) const;

    bool has_pending_draws() const { return !pending_draws_.empty(); }

    /* Drains currently pending HD model draw calls (marking those entities as
     * already drawn this frame so remaining suppressed triangles stay suppressed). */
    std::vector<HdModelDrawCall> take_pending_draws();

    /* Advances frame state at scanout (rotates GTE capture snapshot for double-buffered OT). */
    void end_frame();

    /* Resets per-pass draw/cluster state while preserving GTE bone snapshots and VRAM hashes
     * (used when Smooth Motion restores a VRAM checkpoint to replay a frame). */
    void begin_replay_pass();

    /* Diagnostic/testing accessors. */
    size_t captured_run_count() const;
    uint32_t suppressed_count_this_frame() const { return suppressed_this_frame_; }

private:
    struct SxyRecord {
        uint32_t run_index = 0;
        uint32_t packed_sxy = 0;
        uint16_t sz = 0;
        uint32_t next = UINT32_MAX;
    };
    struct CaptureBuffer {
        SceneContext scene{};
        std::vector<GteTransformRun> runs;
        std::vector<SxyRecord> sxy_pool;
        std::unordered_map<uint32_t, uint32_t> sxy_head;
        void clear() {
            runs.clear();
            sxy_pool.clear();
            sxy_head.clear();
        }
    };
    struct VramUpload {
        unsigned x = 0, y = 0, w = 0, h = 0;
        uint64_t hash = 0;
    };
    static constexpr int kHistorySlots = 6;

    struct ClusterState {
        uint32_t first_run = 0;
        uint32_t run_count = 0;
        int capture_slot = 0; /* 0 = snap_current_, 1..kHistorySlots = snap_history_[slot-1], -100 = cached_poses_ */
        size_t cached_index = SIZE_MAX;
        bool evaluated = false;
        bool matched = false;
        bool flushed = false;
        bool pose_cached = false;
        std::string model_id;
        std::shared_ptr<const HdModel> model;
        float draw_offset_x = 0.0f;
        float draw_offset_y = 0.0f;
        double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;
        uint32_t color_samples = 0;
        size_t pending_index = SIZE_MAX;
        std::unordered_map<std::string, uint32_t> model_votes;
    };
    struct CachedPose {
        std::string model_id;
        std::shared_ptr<const HdModel> model;
        std::vector<GteBoneMatrix> bones;
        float ofx = 0.0f, ofy = 0.0f, h = 0.0f;
        std::unordered_set<uint64_t> tex_hashes;
        std::unordered_map<uint32_t, uint16_t> sxy_to_sz;
        int min_sx = 100000, min_sy = 100000, max_sx = -100000, max_sy = -100000;
        uint64_t last_live_frame = 0;
        uint64_t last_seen_frame = 0;
        uint32_t frame_cluster_id = UINT32_MAX;
    };

    static constexpr int kCachedSlot = kHistorySlots + 1;

    void ensure_snapshot_ready();
    const CaptureBuffer &capture_for_slot(int slot) const;
    bool find_projection(const CaptureBuffer &cap, int vx, int vy, int ox, int oy, SxyRecord &out,
                         uint32_t prefer_run = UINT32_MAX) const;
    uint32_t find_or_create_cluster(int cap_slot, uint32_t run_idx, float draw_ox, float draw_oy);
    int evaluate_run_compat(int cap_slot, uint32_t run_idx, const std::string &tri_model_id) const;
    void evaluate_cluster_match(ClusterState &cluster, uint64_t tex_hash, ModelRegistry &registry);
    void cache_live_cluster(ClusterState &cluster, uint64_t tex_hash);
    void invalidate_cached_poses_for_hash(uint64_t hash);
    static bool bboxes_overlap(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1);

    mutable std::mutex capture_mutex_;
    CaptureBuffer live_capture_;
    CaptureBuffer snap_current_;
    std::array<CaptureBuffer, kHistorySlots> snap_history_;
    bool snapshot_pulled_ = false;

    std::vector<VramUpload> vram_uploads_;
    std::vector<ClusterState> clusters_;
    std::vector<CachedPose> cached_poses_;
    std::unordered_map<uint64_t, uint32_t> run_to_cluster_;
    std::unordered_set<std::string> claimed_scene_ids_;
    std::unordered_map<std::string, uint32_t> frame_max_sup_by_id_;
    std::unordered_map<std::string, std::array<float, 3>> last_tint_by_id_;
    std::vector<HdModelDrawCall> pending_draws_;
    SceneMode last_scene_mode_ = SceneMode::Unknown;
    uint16_t last_field_id_ = 0;
    uint64_t frame_counter_ = 1;
    bool have_last_offset_ = false;
    int last_draw_ox_ = 0;
    int last_draw_oy_ = 0;
    uint32_t suppressed_this_frame_ = 0;
};

} // namespace HostHD

#endif
