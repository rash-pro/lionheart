/* Runs the actual GUI, presenters and service client on synthetic host memory. */
#include "gui_internal.h"
#include "cpu.h"
#include "display.h"
#include "ra.h"
#include "osd.h"
#include "player_paths.h"
#ifdef FF8_VULKAN
#include "vulkan_renderer.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static host_options_t options;
static host_gui_result_t loop(void *context,int shell) { (void)context; return host_display_gui_loop(shell); }
static void changed(void *context,const char *key,int overlay) {
    (void)context; (void)overlay;
    if (!strcmp(key,"ra")) host_ra_enable(options.ra);
    if (!strcmp(key,"ra-notify")) host_ra_notify(options.ra_notify);
}
int main(int argc,char **argv) {
    if (argc!=8 || strcmp(argv[1],"--gui-test")) return 2;
    const char *script=argv[2],*data=argv[3],*disc=argv[4],*renderer=argv[5];
    unsigned width=0,height=0;
    if (sscanf(argv[6],"%ux%u",&width,&height)!=2) return 2;
    unsigned unlock_at=(unsigned)strtoul(argv[7],NULL,10);
    cpu_t *cpu=calloc(1,sizeof(*cpu)); gpu_t *gpu=gpu_create();
    if (!cpu || !gpu || !host_player_directory(data)) return 2;
    host_options_defaults(&options);
    snprintf(options.config_path,sizeof(options.config_path),"%s/lionheart.ini",data);
    const char *enabled=getenv("LIONHEART_RA_GUI_ENABLED"); options.ra=enabled && !strcmp(enabled,"1");
    host_gui_services_t services={0}; services.loop=loop; services.changed=changed;
    host_ra_configure(data,options.ra,1);
    host_ra_frame(cpu); host_ra_disc(disc);
    if (!host_gui_configure(&options,data,&services,script)) return 2;
    host_gui_attach_game();
#ifdef FF8_VULKAN
    if (!host_gpu_configure(renderer,1,0,1,NULL,NULL)) return 2;
    host_gpu_present_capture(NULL,width,height);
#else
    if (strcmp(renderer,"soft")) return 2;
#endif
    if (!host_display_init(1,NULL,NULL) || !host_display_present_capture(NULL,width,height)) return 2;
#ifdef FF8_VULKAN
    if (!host_display_gpu(gpu)) return 2;
#endif
    if (!host_display_gui_attach()) return 2;
    host_display_hotkeys(0,1); host_osd_configure(1,NULL);
    for (unsigned frame=0;frame<2000 && host_display_running() && !host_gui_test_failed() && !host_gui_test_finished();++frame) {
        cpu->ram[0x100]=(uint8_t)(unlock_at && frame>=unlock_at ? 7 : 5);
        host_ra_frame(cpu); host_present(gpu); SDL_Delay(4);
    }
    int failed=host_gui_test_failed() || !host_gui_test_finished();
    host_gui_shutdown(); host_display_shutdown(gpu); host_ra_shutdown();
    free(cpu); free(gpu); return failed ? 2 : 0;
}
