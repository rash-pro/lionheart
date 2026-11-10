// Hand-derived GP0/GP1 cases; no disc, generated code, or emulator reference images.
//
// Rules: https://psx-spx.consoledev.net/graphicsprocessingunitgpu/
// Triangle samples are integer positions with top/left inclusive, bottom/right exclusive.
#include "lh_test.h"
#include "oracles/png.h"
#include "py_random.h"
#include "values.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

namespace {

using Words = std::initializer_list<uint32_t>;
using Grid = std::vector<std::vector<uint16_t>>;

uint32_t xy(int x, int y) { return (static_cast<uint32_t>(x) & 0xFFFF) | ((static_cast<uint32_t>(y) & 0xFFFF) << 16); }
uint16_t rgb5(unsigned r, unsigned g = 0, unsigned b = 0) { return static_cast<uint16_t>(r | (g << 5) | (b << 10)); }

Grid repeated(const std::vector<uint16_t> &row, size_t count) { return Grid(count, row); }

std::string describe(const Grid &grid) {
    std::ostringstream out;
    for (const auto &row : grid) {
        out << " ";
        for (uint16_t value : row) out << " " << value;
        out << "\n";
    }
    return out.str();
}

#define GRID_EQ(actual, expected)                                                                  \
    do {                                                                                           \
        const Grid lh_actual = (actual), lh_expected = (expected);                                 \
        if (lh_actual != lh_expected) lh::fail_at(__FILE__, __LINE__, "image differs\nactual:\n" +  \
                                                  describe(lh_actual) + "expected:\n" + describe(lh_expected)); \
    } while (0)

// The runtime's C interface, with ctypes' declarations as C++ types.
struct Api {
    explicit Api(const lh::Library &lib)
        : create(lib.fn<void *()>("gpu_create")),
          free_gpu(lib.fn<void(void *)>("test_gpu_free")),
          gp0_word(lib.fn<void(void *, uint32_t)>("gpu_gp0")),
          gp1_word(lib.fn<void(void *, uint32_t)>("gpu_gp1")),
          gp0_words(lib.fn<void(void *, const uint8_t *, unsigned)>("gpu_gp0_words")),
          read(lib.fn<uint32_t(void *)>("gpu_read")),
          status(lib.fn<uint32_t(void *)>("gpu_status")),
          vblank(lib.fn<void(void *)>("gpu_vblank")),
          feed(lib.fn<void(void *, const uint32_t *, size_t)>("test_gpu_feed")),
          pixel(lib.fn<uint16_t(void *, unsigned, unsigned)>("test_gpu_pixel")),
          vram(lib.fn<const uint16_t *(void *)>("gpu_vram")),
          display_area(lib.fn<void(void *, int *, int *, int *, int *, int *, int *)>("gpu_display")),
          display_rgb(lib.fn<int(void *, uint8_t *, size_t, int *, int *)>("host_display_rgb")),
          display_dump(lib.fn<int(void *, const char *)>("host_display_dump")),
          display_init(lib.fn<int(int, const char *, const char *)>("host_display_init")),
          present(lib.fn<void(void *)>("host_present")),
          display_running(lib.fn<int()>("host_display_running")),
          display_shutdown(lib.fn<int(void *)>("host_display_shutdown")) {}

    void *(*create)();
    void (*free_gpu)(void *);
    void (*gp0_word)(void *, uint32_t);
    void (*gp1_word)(void *, uint32_t);
    void (*gp0_words)(void *, const uint8_t *, unsigned);
    uint32_t (*read)(void *);
    uint32_t (*status)(void *);
    void (*vblank)(void *);
    void (*feed)(void *, const uint32_t *, size_t);
    uint16_t (*pixel)(void *, unsigned, unsigned);
    const uint16_t *(*vram)(void *);
    void (*display_area)(void *, int *, int *, int *, int *, int *, int *);
    int (*display_rgb)(void *, uint8_t *, size_t, int *, int *);
    int (*display_dump)(void *, const char *);
    int (*display_init)(int, const char *, const char *);
    void (*present)(void *);
    int (*display_running)();
    int (*display_shutdown)(void *);
};

struct Rgb {
    int w, h;
    std::string bytes;
};
class Gpu {
public:
    explicit Gpu(lh::Case &t) : api(t.library("gpu")), g(api.create()) {
        LH_CHECK(g);
        gp0({0xE3000000, 0xE4000000 | 1023 | (511 << 10)});
    }
    ~Gpu() { api.free_gpu(g); }
    Gpu(const Gpu &) = delete;
    Gpu &operator=(const Gpu &) = delete;

    void gp0(Words words) { api.feed(g, words.begin(), words.size()); }
    void gp1(uint32_t word) { api.gp1_word(g, word); }
    uint16_t pixel(int x, int y) { return api.pixel(g, static_cast<unsigned>(x), static_cast<unsigned>(y)); }
    uint32_t status() { return api.status(g); }

    Grid image(int x, int y, int w, int h) {
        Grid grid(static_cast<size_t>(h), std::vector<uint16_t>(static_cast<size_t>(w)));
        for (int j = 0; j < h; ++j) {
            for (int i = 0; i < w; ++i) grid[static_cast<size_t>(j)][static_cast<size_t>(i)] = pixel(x + i, y + j);
        }
        return grid;
    }

    void upload(int x, int y, int w, int h, std::vector<uint32_t> pixels) {
        LH_EQ(pixels.size(), static_cast<size_t>((w ? w : 1024) * (h ? h : 512)));
        if (pixels.size() & 1) pixels.push_back(0xDEAD);
        std::vector<uint32_t> words = {0xA0000000, xy(x, y), xy(w, h)};
        for (size_t i = 0; i < pixels.size(); i += 2) words.push_back(pixels[i] | (pixels[i + 1] << 16));
        api.feed(g, words.data(), words.size());
    }

    void display(int w = 4, int h = 2, bool depth = false, int x = 0, int y = 0) {
        gp1(0x03000000);
        gp1(0x05000000u | static_cast<uint32_t>(x) | (static_cast<uint32_t>(y) << 10));
        gp1(0x06000000u | 512 | (static_cast<uint32_t>(512 + w * 10) << 12));
        gp1(0x07000000u | 16 | (static_cast<uint32_t>(16 + h) << 10));
        gp1(0x08000000u | (depth ? 16 : 0));
    }

    Rgb rgb() {
        std::vector<uint8_t> out(1024 * 512 * 3);
        int w = 0, h = 0;
        LH_CHECK(api.display_rgb(g, out.data(), out.size(), &w, &h));
        return {w, h, std::string(reinterpret_cast<const char *>(out.data()), static_cast<size_t>(w * h * 3))};
    }

    Api api;
    void *g;
};

void check_rgb(const Rgb &actual, int w, int h, const std::string &bytes) {
    LH_EQ(actual.w, w);
    LH_EQ(actual.h, h);
    LH_BYTES_EQ(actual.bytes, bytes);
}

void record_png(const lh::Case &t, const lh::fs::path &path, const lh::oracle::PngImage &image) {
    const std::string bytes = lh::read_file(path);
    const auto size = static_cast<uint32_t>(bytes.size()), rgb_size = static_cast<uint32_t>(image.rgb.size());

}

lh::oracle::PngImage read_png(const lh::Case &t, const lh::fs::path &path) {
    const auto image = lh::oracle::read_png(lh::read_file(path));
    record_png(t, path, image);
    return image;
}

void check_png(const lh::oracle::PngImage &image, uint32_t w, uint32_t h, const std::string &rgb) {
    LH_EQ(image.width, w);
    LH_EQ(image.height, h);
    LH_BYTES_EQ(image.rgb, rgb);
}

std::string repeat_bytes(std::initializer_list<int> values, size_t count) {
    std::string item;
    for (int value : values) item.push_back(static_cast<char>(value));
    std::string out;
    for (size_t i = 0; i < count; ++i) out += item;
    return out;
}

struct OrderRow {
    const char *id;
    std::array<int, 3> order;
};
const OrderRow kOrders[] = {{"order0", {0, 1, 2}}, {"order1", {0, 2, 1}}, {"order2", {1, 0, 2}},
                            {"order3", {1, 2, 0}}, {"order4", {2, 0, 1}}, {"order5", {2, 1, 0}}};

struct RectangleRow {
    const char *id;
    int size;
    uint32_t op;
};
const RectangleRow kRectangles[] = {{"1-104", 1, 0x68}, {"8-112", 8, 0x70}, {"16-120", 16, 0x78}};

struct BoolRow {
    const char *id;
    bool value;
};
const BoolRow kGouraud[] = {{"False", false}, {"True", true}};
const BoolRow kHeadless[] = {{"True", true}, {"False", false}};

struct SemiRow {
    const char *id;
    uint32_t mode;
    std::array<unsigned, 3> expected;
};
const SemiRow kSemi[] = {{"0-expected0", 0, {10, 21, 15}},
                         {"1-expected1", 1, {20, 31, 31}},
                         {"2-expected2", 2, {0, 18, 0}},
                         {"3-expected3", 3, {10, 31, 7}}};

struct DisplayRow {
    const char *id;
    uint32_t mode;
    int width, divisor;
};
const DisplayRow kDisplays[] = {{"0-256", 0, 256, 10}, {"1-320", 1, 320, 8}, {"2-512", 2, 512, 5},
                                {"3-640", 3, 640, 4},  {"64-368", 64, 368, 7}};

const int kChunks[] = {1, 2, 3, 7, 255};

} // namespace

LH_TEST_P(test_triangle_top_left_all_windings, kOrders) {
    Gpu gpu(t);
    const uint32_t vertices[] = {xy(1, 1), xy(5, 1), xy(1, 5)};
    gpu.gp0({0x200000FF, vertices[p.order[0]], vertices[p.order[1]], vertices[p.order[2]]});
    // x+y < 6, x/y >= 1: ten pixels; the descending diagonal is the right edge.
    GRID_EQ(gpu.image(0, 0, 6, 6), (Grid{
        {0, 0, 0, 0, 0, 0},
        {0, 31, 31, 31, 31, 0},
        {0, 31, 31, 31, 0, 0},
        {0, 31, 31, 0, 0, 0},
        {0, 31, 0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0},
    }));
}

LH_TEST(test_quad_shared_edge_blends_once) {
    Gpu gpu(t);
    gpu.gp0({0xE1000020});  // additive
    gpu.gp0({0x2A000040, xy(1, 1), xy(5, 1), xy(1, 5), xy(5, 5)});
    Grid expected = repeated({8, 8, 8, 8, 0}, 4);
    expected.push_back({0, 0, 0, 0, 0});
    GRID_EQ(gpu.image(1, 1, 5, 5), expected);
}

LH_TEST(test_gouraud_affine_samples_and_packet_fifo) {
    Gpu gpu(t);
    gpu.gp0({0x300000F8, xy(0, 0), 0x0000F800, xy(4, 0), 0x00F80000});
    LH_EQ(gpu.pixel(0, 0), 0);  // incomplete packet cannot render
    LH_CHECK(!(gpu.status() & (1u << 26)));
    gpu.gp0({xy(0, 4)});
    LH_EQ(gpu.pixel(0, 0), rgb5(31));
    LH_EQ(gpu.pixel(2, 0), rgb5(15, 15));
    LH_EQ(gpu.pixel(1, 1), rgb5(15, 7, 7));
    LH_CHECK(gpu.status() & (1u << 26));
}

LH_TEST(test_drawing_clip_offset_and_size_rejection) {
    Gpu gpu(t);
    gpu.gp0({0xE3000000 | 3 | (3 << 10), 0xE4000000 | 4 | (4 << 10)});
    gpu.gp0({0xE5000000 | 5 | (5 << 11)});
    gpu.gp0({0x6000FF00, xy(-3, -3), xy(5, 5)});
    GRID_EQ(gpu.image(2, 2, 4, 4), (Grid{{0, 0, 0, 0}, {0, 992, 992, 0}, {0, 992, 992, 0}, {0, 0, 0, 0}}));
    gpu.gp0({0x200000FF, xy(-600, 0), xy(600, 0), xy(0, 100)});
    LH_EQ(gpu.pixel(3, 3), 992);
}

LH_TEST_P(test_fixed_rectangles, kRectangles) {
    Gpu gpu(t);
    gpu.gp0({(p.op << 24) | 0xFF0000, xy(7, 8)});
    GRID_EQ(gpu.image(7, 8, p.size, p.size),
            repeated(std::vector<uint16_t>(static_cast<size_t>(p.size), 0x7C00), static_cast<size_t>(p.size)));
    LH_EQ(gpu.pixel(7 + p.size, 8), 0);
    LH_EQ(gpu.pixel(7, 8 + p.size), 0);
}

LH_TEST(test_clut4_transparent_and_black_texels) {
    Gpu gpu(t);
    gpu.upload(640, 256, 1, 1, {0x3210});
    gpu.upload(512, 300, 4, 1, {0, 31, 992, 0x8000});
    gpu.upload(0, 0, 4, 1, {0x7C00, 0x7C00, 0x7C00, 0x7C00});
    gpu.gp0({0xE100001A});  // x=640, y=256, 4-bit
    gpu.gp0({0x65000000, xy(0, 0), (300u * 64 + 32) << 16, xy(4, 1)});
    GRID_EQ(gpu.image(0, 0, 4, 1), (Grid{{0x7C00, 31, 992, 0x8000}}));
}

LH_TEST(test_clut8_wrap_and_texture_window) {
    Gpu gpu(t);
    gpu.upload(640, 256, 1, 1, {0x0100});
    gpu.upload(1008, 300, 1, 1, {31});
    gpu.upload(1009, 300, 1, 1, {992});
    gpu.gp0({0xE100009A});
    // Mask bit 3 away, sampling u=8 and 9 as u=0 and 1.
    gpu.gp0({0xE2000001});
    gpu.gp0({0x65000000, xy(0, 0), ((300u * 64 + 63) << 16) | 8, xy(2, 1)});
    GRID_EQ(gpu.image(0, 0, 2, 1), (Grid{{31, 992}}));
    gpu.upload(640, 256, 1, 1, {16});
    gpu.upload(0, 300, 1, 1, {0x7C00});
    gpu.gp0({0x65000000, xy(3, 0), (300u * 64 + 63) << 16, xy(1, 1)});
    LH_EQ(gpu.pixel(3, 0), 0x7C00);
}

LH_TEST(test_texture_window_offset_and_flip) {
    Gpu gpu(t);
    gpu.upload(648, 256, 2, 1, {31, 992});
    gpu.gp0({0xE100011A, 0xE2000001 | (1 << 10)});
    gpu.gp0({0x65000000, xy(0, 0), 0, xy(2, 1)});
    GRID_EQ(gpu.image(0, 0, 2, 1), (Grid{{31, 992}}));
    gpu.gp0({0xE100111A});
    gpu.gp0({0x65000000, xy(2, 0), 1, xy(2, 1)});
    GRID_EQ(gpu.image(2, 0, 2, 1), (Grid{{992, 31}}));
}

LH_TEST_P(test_textured_quad_packet_tpage_and_modulation, kGouraud) {
    Gpu gpu(t);
    gpu.upload(640, 256, 2, 2, {31, 992, 0x7C00, 0x7FFF});
    const uint32_t mode = 0x11A;
    gpu.gp0({0xE1000200});  // tpage in polygon must preserve dither
    std::vector<uint32_t> words = {p.value ? 0x3C808080u : 0x2C808080u, xy(0, 0), 0};
    const struct { uint32_t coord, uv; } corners[] = {
        {xy(2, 0), (mode << 16) | 2}, {xy(0, 2), 2u << 8}, {xy(2, 2), (2u << 8) | 2}};
    for (const auto &corner : corners) {
        if (p.value) words.push_back(0x808080);
        words.push_back(corner.coord);
        words.push_back(corner.uv);
    }
    gpu.api.feed(gpu.g, words.data(), words.size());
    LH_EQ(gpu.status() & 0x3FF, mode | 512);
    // Dither subtracts 4/3 from full-scale channels at x=0, hence 30 there.
    GRID_EQ(gpu.image(0, 0, 2, 2), (Grid{{30, 992}, {0x7C00, rgb5(30, 30, 30)}}));
}

LH_TEST_P(test_semitransparency_modes, kSemi) {
    Gpu gpu(t);
    gpu.upload(0, 0, 1, 1, {rgb5(7, 30, 0)});
    gpu.gp0({0xE1000000 | (p.mode << 5)});
    gpu.gp0({0x6A000000 | (13 * 8) | (12 * 8 << 8) | (31 * 8 << 16), xy(0, 0)});
    LH_EQ(gpu.pixel(0, 0), rgb5(p.expected[0], p.expected[1], p.expected[2]));
}

LH_TEST(test_texture_semitransparency_bit_and_raw_color) {
    Gpu gpu(t);
    gpu.upload(640, 256, 2, 1, {0x8008, 8});
    gpu.upload(0, 0, 2, 1, {8, 8});
    gpu.gp0({0xE100013A, 0x67000000, xy(0, 0), 0, xy(2, 1)});
    GRID_EQ(gpu.image(0, 0, 2, 1), (Grid{{0x8010, 8}}));
}

LH_TEST(test_modulation_neutral_double_and_half) {
    Gpu gpu(t);
    gpu.upload(640, 256, 1, 1, {rgb5(16, 8, 4)});
    gpu.gp0({0xE100011A});
    const uint32_t colors[] = {0x808080, 0xFFFFFF, 0x404040};
    for (int x = 0; x < 3; ++x) gpu.gp0({0x64000000 | colors[x], xy(x, 0), 0, xy(1, 1)});
    GRID_EQ(gpu.image(0, 0, 3, 1), (Grid{{rgb5(16, 8, 4), rgb5(31, 15, 7), rgb5(8, 4, 2)}}));
}

LH_TEST(test_mask_set_check_on_draw_upload_and_copy) {
    Gpu gpu(t);
    gpu.gp0({0xE6000001, 0x680000FF, xy(0, 0)});
    LH_EQ(gpu.pixel(0, 0), 0x801F);
    gpu.gp0({0xE6000002, 0x6800FF00, xy(0, 0)});
    gpu.upload(0, 0, 2, 1, {0x7C00, 992});
    GRID_EQ(gpu.image(0, 0, 2, 1), (Grid{{0x801F, 992}}));
    gpu.gp0({0x80000000, xy(1, 0), xy(0, 0), xy(1, 1)});
    LH_EQ(gpu.pixel(0, 0), 0x801F);
    gpu.gp0({0xE6000001, 0x80000000, xy(1, 0), xy(2, 0), xy(1, 1)});
    LH_EQ(gpu.pixel(2, 0), 0x83E0);
}

LH_TEST(test_fill_alignment_mask_bypass_and_zero_sizes) {
    Gpu gpu(t);
    gpu.gp0({0xE6000003});
    gpu.upload(16, 0, 1, 1, {0xFFFF});
    gpu.gp0({0x020000FF, xy(19, 0), xy(1, 1)});
    std::vector<uint16_t> row(18, 31);
    row.front() = row.back() = 0;
    GRID_EQ(gpu.image(15, 0, 18, 1), (Grid{row}));
    gpu.gp0({0x0200FF00, xy(16, 0), xy(1024, 1), 0x0200FF00, xy(16, 0), xy(16, 512)});
    LH_EQ(gpu.pixel(16, 0), 31);
}

LH_TEST(test_transfer_wrap_odd_padding_and_copy) {
    Gpu gpu(t);
    gpu.upload(1023, 511, 3, 2, {1, 2, 3, 4, 5, 6});
    std::vector<uint16_t> seen;
    for (int y : {511, 0}) for (int x : {1023, 0, 1}) seen.push_back(gpu.pixel(x, y));
    LH_SEQ_EQ(seen, (std::vector<uint16_t>{1, 2, 3, 4, 5, 6}), "");
    gpu.gp0({0x80000000, xy(1023, 511), xy(20, 20), xy(3, 2)});
    GRID_EQ(gpu.image(20, 20, 3, 2), (Grid{{1, 2, 3}, {4, 5, 6}}));
    gpu.upload(30, 30, 3, 1, {7, 8, 9});
    LH_EQ(gpu.pixel(33, 30), gpu.pixel(30, 31));
    LH_EQ(gpu.pixel(33, 30), 0);
    gpu.gp0({0xC0000000, xy(30, 30), xy(3, 1)});
    gpu.gp1(0x04000003);
    LH_CHECK((gpu.status() & (1u << 27)) && (gpu.status() & (1u << 25)));
    LH_EQ(gpu.api.read(gpu.g), 0x00080007u);
    LH_EQ(gpu.api.read(gpu.g) & 0xFFFF, 9u);
    LH_CHECK(!(gpu.status() & (1u << 27)) && !(gpu.status() & (1u << 25)));
    LH_CHECK(gpu.status() & (1u << 26));
}

LH_TEST(test_zero_transfer_width_means_1024) {
    Gpu gpu(t);
    gpu.upload(0, 0, 0, 1, std::vector<uint32_t>(1024, 31));
    GRID_EQ(gpu.image(0, 0, 1024, 1), repeated(std::vector<uint16_t>(1024, 31), 1));
    LH_EQ(gpu.pixel(0, 1), 0);
}

LH_TEST(test_overlapping_copy_preserves_small_source_row) {
    Gpu gpu(t);
    gpu.upload(0, 0, 5, 1, {1, 2, 3, 4, 5});
    gpu.gp0({0x80000000, xy(0, 0), xy(1, 0), xy(4, 1)});
    GRID_EQ(gpu.image(0, 0, 5, 1), (Grid{{1, 1, 2, 3, 4}}));
}

LH_TEST(test_lines_polyline_terminators_and_joints) {
    Gpu gpu(t);
    gpu.gp0({0xE1000020, 0x4A000040, xy(1, 1), xy(3, 1), xy(3, 3), 0x50005000});
    GRID_EQ(gpu.image(1, 1, 3, 3), (Grid{{8, 8, 8}, {0, 0, 8}, {0, 0, 8}}));
    gpu.gp0({0x580000F8, xy(0, 5), 0x0000F800, xy(2, 5), 0x00F80000, xy(4, 5), 0x5ABC5DEF});
    GRID_EQ(gpu.image(0, 5, 5, 1), (Grid{{31, rgb5(15, 15), 992, rgb5(0, 15, 15), 0x7C00}}));
    gpu.gp0({0x400000FF, xy(6, 6), xy(6, 6)});
    LH_EQ(gpu.pixel(6, 6), 31);
    LH_CHECK(gpu.status() & (1u << 26));
}

LH_TEST(test_dither_matrix_and_primitive_rules) {
    Gpu gpu(t);
    gpu.gp0({0xE1000200});
    gpu.gp0({0x38808080, xy(0, 0), 0x808080, xy(4, 0), 0x808080, xy(0, 4), 0x808080, xy(4, 4)});
    const unsigned matrix[4][4] = {{15, 16, 15, 16}, {16, 15, 16, 15}, {15, 16, 15, 16}, {16, 15, 16, 15}};
    Grid expected;
    for (const auto &row : matrix) {
        std::vector<uint16_t> values;
        for (unsigned n : row) values.push_back(rgb5(n, n, n));
        expected.push_back(values);
    }
    GRID_EQ(gpu.image(0, 0, 4, 4), expected);
    gpu.gp0({0x60808080, xy(4, 0), xy(4, 4)});
    GRID_EQ(gpu.image(4, 0, 4, 4), repeated(std::vector<uint16_t>(4, rgb5(16, 16, 16)), 4));
    gpu.gp0({0x20808080, xy(8, 0), xy(12, 0), xy(8, 4)});
    LH_EQ(gpu.pixel(8, 0), rgb5(16, 16, 16));
    gpu.gp0({0x40808080, xy(0, 8), xy(3, 8)});
    std::vector<uint16_t> line;
    for (unsigned n : {15u, 16u, 15u, 16u}) line.push_back(rgb5(n, n, n));
    GRID_EQ(gpu.image(0, 8, 4, 1), (Grid{line}));
}

LH_TEST(test_reset_info_latch_irq_and_dma_polling) {
    Gpu gpu(t);
    gpu.gp1(0);
    LH_EQ(gpu.status(), 0x14802000u);
    gpu.gp0({0xE2123456});
    gpu.gp1(0x10000002);
    LH_EQ(gpu.api.read(gpu.g), 0x23456u);
    gpu.gp0({0xE2000000});
    LH_EQ(gpu.api.read(gpu.g), 0x23456u);
    gpu.gp1(0x11000007);
    LH_EQ(gpu.api.read(gpu.g), 2u);
    gpu.gp0({0x1F000000});
    LH_CHECK(gpu.status() & (1u << 24));
    gpu.gp1(0x02000000);
    LH_CHECK(!(gpu.status() & (1u << 24)));
    for (uint32_t dma = 0; dma < 4; ++dma) {
        gpu.gp1(0x04000000u | dma);
        LH_EQ((gpu.status() >> 29) & 3, dma);
        LH_EQ(static_cast<bool>(gpu.status() & (1u << 25)), dma == 1 || dma == 2);
    }
    gpu.gp0({0xA0000000, xy(0, 0), xy(4, 4), 0x12345678});
    LH_CHECK(!(gpu.status() & (1u << 26)));
    gpu.gp1(0x01000000);
    LH_CHECK(gpu.status() & (1u << 26));
    LH_EQ(gpu.pixel(0, 0), 0x5678);
    gpu.gp1(0);
    LH_EQ(gpu.pixel(0, 0), 0x5678);  // Reset does not erase VRAM.
}

LH_TEST_P(test_display_ranges_resolution_and_interlace, kDisplays) {
    Gpu gpu(t);
    gpu.gp1(0x06000000u | 512 | (static_cast<uint32_t>(512 + p.width * p.divisor) << 12));
    gpu.gp1(0x07000000u | 16 | ((16u + 240) << 10));
    gpu.gp1(0x08000000u | p.mode | 36);
    int values[6] = {0, 0, 0, 0, 0, 0};
    gpu.api.display_area(gpu.g, &values[0], &values[1], &values[2], &values[3], &values[4], &values[5]);
    LH_EQ(values[2], p.width);
    LH_EQ(values[3], 480);
    const uint32_t before = gpu.status();
    gpu.api.vblank(gpu.g);
    LH_EQ((before ^ gpu.status()) & ((1u << 31) | (1u << 13)), (1u << 31) | (1u << 13));
}

LH_TEST(test_display_24bit_byte_order_origin_wrap_and_disable) {
    Gpu gpu(t);
    std::string row;
    std::vector<uint32_t> pixels;
    for (int i = 0; i < 12; ++i) row.push_back(static_cast<char>(i));
    for (int i = 0; i < 12; i += 2) pixels.push_back(static_cast<uint32_t>(i) | (static_cast<uint32_t>(i + 1) << 8));
    gpu.upload(1023, 511, 6, 1, pixels);
    gpu.display(4, 1, true, 1023, 511);
    check_rgb(gpu.rgb(), 4, 1, row);
    gpu.gp1(0x03000001);
    check_rgb(gpu.rgb(), 4, 1, std::string(12, '\0'));
}

LH_TEST(test_display_15bit_conversion_and_wrapped_rows) {
    Gpu gpu(t);
    gpu.upload(1023, 511, 4, 2, {31, 992, 0x7C00, 0xFFFF, 0, 0, 0, 0});
    gpu.display(4, 2, false, 1023, 511);
    std::string expected = repeat_bytes({255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255}, 1);
    expected += std::string(12, '\0');
    check_rgb(gpu.rgb(), 4, 2, expected);
}

LH_TEST_P(test_present_pngs_and_vram_exit, kHeadless) {
    Gpu gpu(t);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    const auto frames = t.temp() / "nested/frames", vram = t.temp() / "vram.png";
    gpu.display(4, 2);
    gpu.upload(0, 0, 4, 2, std::vector<uint32_t>(8, 31));
    LH_CHECK(gpu.api.display_init(p.value, frames.c_str(), vram.c_str()));
    bool shutdown_ok = false;
    try {
        for (int i = 0; i < 2; ++i) {
            gpu.api.vblank(gpu.g);
            gpu.api.present(gpu.g);
        }
        LH_CHECK(gpu.api.display_running());
    } catch (...) {
        gpu.api.display_shutdown(gpu.g);
        throw;
    }
    shutdown_ok = gpu.api.display_shutdown(gpu.g) != 0;
    LH_CHECK(shutdown_ok);
    std::vector<std::string> names;
    for (const auto &entry : lh::fs::directory_iterator(frames)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    LH_EQ(names.size(), 2u);
    LH_EQ(names[0], std::string("frame-000000.png"));
    LH_EQ(names[1], std::string("frame-000001.png"));
    check_png(read_png(t, frames / "frame-000000.png"), 4, 2, repeat_bytes({255, 0, 0}, 8));
    const auto dump = read_png(t, vram);
    LH_EQ(dump.width, 1024u);
    LH_EQ(dump.height, 512u);
    LH_BYTES_EQ(dump.rgb.substr(0, 12), repeat_bytes({255, 0, 0}, 4));
}

LH_TEST(test_png_output_failure_is_reported) {
    Gpu gpu(t);
    const auto missing = t.temp() / "missing/vram.png";
    LH_CHECK(gpu.api.display_init(1, nullptr, missing.c_str()));
    LH_CHECK(!gpu.api.display_shutdown(gpu.g));
}

LH_TEST(test_truncated_packets_and_random_control_streams) {
    Gpu gpu(t);
    // Exercise bounded packet/polyline/transfer state, including reset mid-command.
    lh::PyRandom rng(0x505358);
    for (int i = 0; i < 1000; ++i) {
        const auto a = static_cast<uint32_t>(rng.getrandbits(32)), b = static_cast<uint32_t>(rng.getrandbits(32));
        gpu.gp0({a, b});
        gpu.gp1(0x01000000);
        const auto command = 0x10000000u | static_cast<uint32_t>(rng.randrange(16));
        gpu.gp1(command);
        gpu.api.read(gpu.g);

    }
    LH_CHECK(gpu.status() & (1u << 26));
}

LH_TEST(test_copy_large_horizontal_overlap_and_vertical_feedback) {
    Gpu gpu(t);
    std::vector<uint32_t> source;
    for (uint32_t i = 1; i <= 300; ++i) source.push_back(i);
    gpu.upload(0, 0, 300, 1, source);
    gpu.gp0({0x80000000, xy(0, 0), xy(1, 0), xy(299, 1)});
    std::vector<uint16_t> expected = {1};
    for (size_t i = 0; i + 1 < source.size(); ++i) expected.push_back(static_cast<uint16_t>(source[i]));
    GRID_EQ(gpu.image(0, 0, 300, 1), (Grid{expected}));
    gpu.upload(0, 10, 2, 3, {1, 2, 3, 4, 5, 6});
    gpu.gp0({0x80000000, xy(0, 10), xy(0, 11), xy(2, 2)});
    GRID_EQ(gpu.image(0, 10, 2, 3), (Grid{{1, 2}, {1, 2}, {1, 2}}));
}

LH_TEST(test_negative_line_coordinates_clip_without_truncation_artifact) {
    Gpu gpu(t);
    gpu.gp0({0x400000FF, xy(-2, -1), xy(2, 1)});
    // The first two samples are offscreen, and (0,0),(1,1),(2,1) remain.
    GRID_EQ(gpu.image(0, 0, 3, 2), (Grid{{31, 0, 0}, {0, 31, 31}}));
}

LH_TEST(test_zero_display_ranges_are_black_one_pixel_png) {
    Gpu gpu(t);
    gpu.gp1(0x06000000);
    gpu.gp1(0x07000000);
    const auto path = t.temp() / "blank.png";
    LH_CHECK(gpu.api.display_dump(gpu.g, path.c_str()));
    check_png(read_png(t, path), 1, 1, std::string(3, '\0'));
}

LH_TEST(test_display_area_write_protection_and_override) {
    Gpu gpu(t);
    gpu.display(4, 2, false, 4, 4);
    gpu.gp0({0x600000FF, xy(3, 3), xy(4, 4)});
    GRID_EQ(gpu.image(3, 3, 4, 4), (Grid{{31, 31, 31, 31}, {31, 0, 0, 0}, {31, 0, 0, 0}, {31, 31, 31, 31}}));
    gpu.gp0({0xE1000400, 0x6800FF00, xy(4, 4)});
    LH_EQ(gpu.pixel(4, 4), 992);
    gpu.gp0({0xE1000000});
    gpu.gp1(0x03000001);
    gpu.gp0({0x680000FF, xy(4, 4)});
    LH_EQ(gpu.pixel(4, 4), 31);
}

LH_TEST(test_interlaced_rendering_field_and_draw_to_display) {
    Gpu gpu(t);
    gpu.display(4, 2);
    gpu.gp1(0x08000024);
    gpu.gp0({0x600000FF, xy(0, 0), xy(2, 4)});
    GRID_EQ(gpu.image(0, 0, 2, 4), (Grid{{0, 0}, {31, 31}, {0, 0}, {31, 31}}));
    gpu.api.vblank(gpu.g);
    gpu.gp0({0x6000FF00, xy(0, 0), xy(2, 4)});
    GRID_EQ(gpu.image(0, 0, 2, 4), (Grid{{992, 992}, {31, 31}, {992, 992}, {31, 31}}));
    gpu.gp0({0xE1000400, 0x60FF0000, xy(0, 0), xy(2, 4)});
    GRID_EQ(gpu.image(0, 0, 2, 4), repeated({0x7C00, 0x7C00}, 4));
}

LH_TEST_P(test_dma_packets_match_word_port, kChunks) {
    // Image upload, odd pixel count, partial commands, and a Gouraud polyline
    // all cross arbitrary DMA packet boundaries.
    const std::vector<uint32_t> words = {0xe3000000, 0xe407ffff, 0xa0000000, 0x00080010, 0x00010003,
                                         0x03e0001f, 0x00007c00, 0x020000ff, 0x00200010, 0x00020010,
                                         0x580000ff, 0x00300020, 0x0000ff00, 0x00340024,
                                         0x00ff0000, 0x00380028, 0x50005000, 0xe6000001};
    const Api api(t.library("gpu"));
    void *left = api.create(), *right = api.create();
    const auto release = [&] { api.free_gpu(left); api.free_gpu(right); };
    try {
        for (uint32_t word : words) api.gp0_word(left, word);
        for (size_t start = 0; start < words.size(); start += static_cast<size_t>(p)) {
            const size_t end = std::min(words.size(), start + static_cast<size_t>(p));
            std::vector<uint8_t> data((end - start) * 4);
            for (size_t i = 0; i < end - start; ++i) {
                for (size_t b = 0; b < 4; ++b) data[i * 4 + b] = static_cast<uint8_t>(words[start + i] >> (8 * b));
            }
            api.gp0_words(right, data.data(), static_cast<unsigned>(data.size() / 4));
        }
        const std::string a(reinterpret_cast<const char *>(api.vram(left)), 1024 * 512 * 2);
        const std::string b(reinterpret_cast<const char *>(api.vram(right)), 1024 * 512 * 2);
        LH_BYTES_EQ(a, b);
        LH_EQ(api.status(left), api.status(right));
    } catch (...) {
        release();
        throw;
    }
    release();
}
