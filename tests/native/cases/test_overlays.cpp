// Overlay extraction facts and executable loader-lifetime regressions.
#include "generated.h"

using namespace lh::gen;

namespace {

LH_TEST(test_effect_address_inference_rejects_assets_and_ambiguity) {
    const std::string error = error_of([] { infer_effect_base(std::string(512, '\0')); });
    LH_CONTAINS(error, "ambiguous");
    // j 0x801a0010; nop; data; data; addiu sp,sp,-16; jr ra; nop
    const std::string data = pack({0x08068004, 0, 0xFFFFFFFF, 0, 0x27BDFFF0, 0x03E00008, 0});
    const EffectBase result = infer_effect_base(data);
    LH_EQ(result.base, 0x801A0000u);
    LH_TEXT_EQ(result.evidence.at("examples").at(0).at("target_offset").str(), "0x10");
}

struct Mode { const char *id; int mode; };
static const Mode kModes[] = {{"cached-miss", 0}, {"nested-return", 1}, {"callback-unwind", 2}, {"cpu-stop", 3},
                              {"return-from-exception", 4}, {"outer-unwind", 5}, {"interpreter-only", 6}};

LH_TEST_P(test_dispatch_pins_native_modules, kModes) {
    t.ok(t.harness("probe").arg(p.mode).timeout(10));
}

LH_TEST(test_overlay_profile_counts_uncompiled_catalog_sets) {
    const lh::fs::path catalog = t.temp() / "catalog.txt";
    const lh::fs::path output = t.temp() / "counts.csv";
    // Mode 6 executes two interpreted guest words without invoking a native hook.
    lh::write_file(catalog, "511 80098000 8 1 80098000 03e00008\n");
    // The dispatch harness returns normally, so use an instruction-budget stop
    // in a separate tiny executable to exercise the normal report flush as well.
    t.ok(t.harness("profile").env("FF8_OVERLAY_CATALOG", catalog).env("FF8_OVERLAY_PROFILE", output));
    LH_TEXT_EQ(lh::read_text_file(output), "index,interpreter,recompiled\n511,2,3\n");
}

} // namespace
