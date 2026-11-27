// Built-in OFL HD font detection, CLUT colorization, sentinel-safe atlas rendering,
// HD 3D model loading, live GTE scene/bone matching, shader contract, and local .zzz / test-pack importer.
#include "host_fixtures.h"
#include "presentation.h"

namespace {
static const char *hd_model_shaders[] = {"hd_model.vert", "hd_model.frag"};
LH_TEST_P(test_hd_model_prebuilt_shader_matches_source_and_validates, hd_model_shaders) {
    lh::shader_contract(t, p);
}

LH_TEST(test_hd_font_detection_clut_and_atlas) {
    auto r = t.ok(t.harness("hd_font").arg(t.root() / "third_party/chakra-petch/ChakraPetch-Regular.ttf").timeout(30));
    LH_CONTAINS(r.out, "ok");
}

LH_TEST(test_hd_models_glb_manifest_and_gte_projection) {
    auto r = t.ok(t.harness("hd_models").arg(t.temp() / "models").timeout(30));
    LH_CONTAINS(r.out, "ok");
}

LH_TEST(test_hd_models_importer_zzz_and_test_pack) {
    auto probe = t.run(lh::Command("python3", false).arg("--version").timeout(10));
    if (probe.rc != 0) lh::skip("python3 is not available on this runner");
    auto tool = t.root() / "tools/hd_models_import.py";
    auto r = t.ok(lh::Command("python3", false).arg(tool).arg("--self-test").timeout(30));
    LH_CONTAINS(r.out, "self-test ok");
    auto pack = t.temp() / "synthetic-pack";
    auto gen = t.ok(lh::Command("python3", false).arg(tool).arg("--make-test-pack").arg(pack).timeout(30));
    LH_CONTAINS(gen.out, "[OK] field/d000");
    auto val = t.ok(lh::Command("python3", false).arg(tool).arg("--validate-pack").arg(pack).timeout(30));
    LH_CONTAINS(val.out, "[OK] battle/c0m001");
    /* Also verify C++ ModelRegistry loads the Python-generated .glb pack cleanly. */
    auto cpp = t.ok(t.harness("hd_models").arg(t.temp() / "cpp-models").arg(pack).timeout(30));
    LH_CONTAINS(cpp.out, "ok");
}

LH_TEST(test_native_mod_importer_dds_xml_and_iros) {
    auto r = t.ok(t.harness("mod_import").arg(t.temp() / "mod-import").timeout(30));
    LH_CONTAINS(r.out, "ok");
}
} // namespace
