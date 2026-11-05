#include "options.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    host_options_t o; host_options_defaults(&o);
    if (!host_options_load(&o, argc, argv)) return 2;
    for (int i = 2; i < argc; ++i) if (host_options_cli(&o, argc, argv, &i) != 1) return 2;
    printf("%d %d %d %d %d\n", o.dualshock, o.rumble, o.fullscreen, o.scale_mode, o.linear);
    for (size_t i = 0; i < host_option_count; ++i) printf("%s %c\n", host_options[i].key, host_options[i].classification);
    if (getenv("FF8_TEST_OPTIONS")) printf("%d %llu %llu %d %s %s %llu\n",o.renderer,(unsigned long long)o.internal_scale,(unsigned long long)o.msaa,o.pgxp,o.movies,o.bridge,(unsigned long long)o.save_at);
    if (getenv("FF8_TEST_OSD")) printf("osd=%d tests=%s",o.osd,o.osd_tests);
    if (getenv("FF8_TEST_HOTKEYS")) printf("hotkey=%d directory=%s frames=%s",o.pad_hotkey,o.screenshot_dir,o.screenshot_at);
    if (getenv("FF8_TEST_CARDS")) printf("cards=%s\n",o.snapshot_cards ? "snapshot" : "live");
    if (getenv("FF8_TEST_INTERPOLATION")) printf("frame-interpolation=%d\n",o.frame_interpolation);
    if (getenv("FF8_TEST_CRT")) printf("crt=%d present=%llux%llu presented=%s\n",o.crt,
        (unsigned long long)(o.present_size & 0xffffffffu),(unsigned long long)(o.present_size >> 32),o.presented_frames);
    return 0;
}
