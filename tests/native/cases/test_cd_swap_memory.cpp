// A disc swap must not fail because optional per-LBA diagnostics exhaust RAM.
#include "lh_test.h"

LH_TEST(test_swap_survives_statistics_allocation_failure) {
    const lh::fs::path images[] = {t.temp() / "before.bin", t.temp() / "after.bin"};
    for (const auto &image : images) {
        lh::write_file(image, "");
        lh::fs::resize_file(image, 100001 * 2352);
    }
    t.ok(t.harness("swap").args({images[0], images[1]}).timeout(20));
}
