// The module loader refuses anything that was not built for this player and this
// executable, without crashing and without registering it. Synthetic modules
// only: the executable is 64 made-up bytes.
#include "lh_test.h"

namespace {
struct Loader {
    lh::Case &t;
    lh::fs::path directory;
    explicit Loader(lh::Case &c) : t(c), directory(lh::fs::path(c.harness("loader").program()).parent_path()) {}
    lh::fs::path module(const std::string &name) const { return directory / (name + ".so"); }
    std::string run(std::initializer_list<lh::fs::path> modules) {
        auto command = t.harness("loader");
        for (const auto &path : modules) command.arg(path);
        return t.ok(command.timeout(30)).out;
    }
};
const char *const kNothing = "registered 0 main 0 overlays 0\n";
}

LH_TEST(accepts_binds_and_runs_matching_modules) {
    Loader loader(t);
    LH_TEXT_EQ(loader.run({loader.module("main"), loader.module("overlay")}),
               "ok \nok \nregistered 2 main 1 overlays 1\nr2 42 stop overlay 7\nafter shutdown 0\n");
}

LH_TEST(refuses_wrong_abi_identity) {
    Loader loader(t);
    const auto out = loader.run({loader.module("overlay_other_abi")});
    LH_CONTAINS(out, "bad-abi ");
    LH_CONTAINS(out, kNothing);
}

LH_TEST(refuses_other_build_settings) {
    Loader loader(t);
    const auto out = loader.run({loader.module("overlay_other_flags")});
    LH_CONTAINS(out, "bad-abi ");
    LH_CONTAINS(out, kNothing);
}

LH_TEST(refuses_wrong_image_identity) {
    Loader loader(t);
    const auto out = loader.run({loader.module("overlay_other_exe")});
    LH_CONTAINS(out, "bad-image generated from another executable");
    LH_CONTAINS(out, kNothing);
}

LH_TEST(refuses_code_that_does_not_match_guest_memory) {
    Loader loader(t);
    const auto out = loader.run({loader.module("main_code_mismatch")});
    LH_CONTAINS(out, "code-mismatch code at 80010000 does not match guest memory");
    LH_CONTAINS(out, kNothing);
}

LH_TEST(refuses_truncated_files) {
    Loader loader(t);
    const std::string whole = lh::read_file(loader.module("overlay"));
    for (size_t size : {size_t(0), size_t(63), whole.size() / 2, whole.size() - 1}) {
        const auto path = t.temp() / ("truncated-" + std::to_string(size) + ".so");
        lh::write_file(path, whole.substr(0, size));
        const auto out = loader.run({path});
        LH_CONTAINS(out, "truncated ");
        LH_CONTAINS(out, kNothing);
    }
}

LH_TEST(refuses_a_missing_import) {
    Loader loader(t);
    // Without main nothing can be imported; with main the pc must be one of its entries.
    auto out = loader.run({loader.module("overlay")});
    LH_CONTAINS(out, "missing-import main-owned function 80010000 is not available");
    LH_CONTAINS(out, kNothing);
    out = loader.run({loader.module("main"), loader.module("overlay_missing_import")});
    LH_CONTAINS(out, "missing-import main-owned function 80010020 is not available");
    LH_CONTAINS(out, "registered 1 main 1 overlays 0\n");
}

LH_TEST(refuses_libraries_without_a_descriptor_or_file) {
    Loader loader(t);
    const auto out = loader.run({loader.module("overlay_no_descriptor"), t.temp() / "absent.so"});
    LH_CONTAINS(out, "no-descriptor ");
    LH_CONTAINS(out, "unreadable ");
    LH_CONTAINS(out, kNothing);
}
