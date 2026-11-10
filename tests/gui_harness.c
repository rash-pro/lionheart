#define _POSIX_C_SOURCE 200809L
#include "gui_internal.h"
#include "sio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv) {
    char path[4096],error[1024]={0};
    if (argc<2) return 64;
    if (!strcmp(argv[1],"runtime-card") && argc==3) {
        memcard_t *card=memcard_open(argv[2],error,sizeof(error));
        if (!card) { fprintf(stderr,"%s\n",error); return 2; }
        memcard_close(card); return 0;
    }
    if (!strcmp(argv[1],"write") && argc==5) {
        host_options_t options; int ok;
        host_options_defaults(&options);
        if (strlen(argv[2])>=sizeof(options.config_path)) return 64;
        strcpy(options.config_path,argv[2]);
        ok=host_options_write(&options,argv[3],argv[4],error,sizeof(error));
        printf("ok=%d fullscreen=%d rumble=%d scale=%llu\n",ok,options.fullscreen,options.rumble,(unsigned long long)options.internal_scale);
        if (!ok) fprintf(stderr,"%s\n",error);
        return ok ? 0 : 2;
    }
    if (!strcmp(argv[1],"scale") && argc==3) {
        printf("%.6f\n",gui_scale((unsigned)strtoul(argv[2],NULL,10))); return 0;
    }
    if (!strcmp(argv[1],"card-summary") && argc==3) {
        gui_card_summary_t summary;
        if (!gui_card_summary(argv[2],&summary)) return 2;
        printf("%u %u\n",summary.blocks_used,summary.ff8_saves); return 0;
    }
    if (!strcmp(argv[1],"card-import") && argc==4) {
        if (!gui_card_import(argv[2],argv[3],path,sizeof(path),error,sizeof(error))) {
            fprintf(stderr,"%s\n",error); return 2;
        }
        puts(path); return 0;
    }
    if (!strcmp(argv[1],"card-new") && argc==4) {
        if (!gui_card_new(argv[2],(unsigned)strtoul(argv[3],NULL,10),path,sizeof(path),error,sizeof(error))) {
            fprintf(stderr,"%s\n",error); return 2;
        }
        puts(path); return 0;
    }
    if (!strcmp(argv[1],"browser") && argc==5) {
        gui_browser_t browser={0};
        if (!gui_browser_open(&browser,argv[2],(gui_pick_t)atoi(argv[3]))) { fprintf(stderr,"%s\n",browser.error); return 2; }
        browser.show_all=atoi(argv[4]);
        if (!gui_browser_refresh(&browser)) { fprintf(stderr,"%s\n",browser.error); return 2; }
        for (unsigned i=0;i<browser.count;++i) printf("%c\t%s\t%llu\n",browser.entries[i].directory ? 'd' : 'f',browser.entries[i].name,(unsigned long long)browser.entries[i].bytes);
        return 0;
    }
    if (!strcmp(argv[1],"browser-accept") && argc==5) {
        gui_browser_t browser={0};
        if (!gui_browser_open(&browser,argv[2],(gui_pick_t)atoi(argv[3]))) return 2;
        printf("%d\n",gui_browser_accept(&browser,argv[4])); return 0;
    }
    if (!strcmp(argv[1],"browser-up") && argc==3) {
        gui_browser_t browser={0};
        if (!gui_browser_open(&browser,argv[2],GUI_PICK_DISC) || !gui_browser_up(&browser)) return 2;
        puts(browser.folder); return 0;
    }
    if (!strcmp(argv[1],"gui-state") && argc==5) {
        int input;
        if (!gui_state_save(argv[2],argv[3],atoi(argv[4]),0) || !gui_state_load(argv[2],path,sizeof(path),&input,NULL)) return 2;
        printf("%d\t%s\n",input,path); return 0;
    }
    if (!strcmp(argv[1],"disc-error") && argc==3) { puts(gui_disc_error_id(argv[2])); return 0; }
    if (!strcmp(argv[1],"native-reason") && argc==3) { puts(gui_native_reason_id(argv[2])); return 0; }
    if (!strcmp(argv[1],"native-summary") && argc==11) {
        /* compiler step done total build sets ready failed seconds_left */
        host_gui_native_t native; char line[256]; int alert;
        memset(&native,0,sizeof(native));
        native.compiler=(host_gui_compiler_t)atoi(argv[2]); native.step=(host_gui_step_t)atoi(argv[3]);
        native.done=strtoull(argv[4],NULL,10); native.total=strtoull(argv[5],NULL,10);
        native.build=(host_gui_build_t)atoi(argv[6]); native.sets=(unsigned)atoi(argv[7]);
        native.ready=(unsigned)atoi(argv[8]); native.failed=(unsigned)atoi(argv[9]); native.seconds_left=atoi(argv[10]);
        snprintf(native.reason,sizeof(native.reason),"Download failed; retry to resume");
        gui_native_summary(&native,line,sizeof(line),&alert);
        printf("%d %s\n",alert,line); return 0;
    }
    if (!strcmp(argv[1],"identify") && argc==4) {
        gui_disc_result_t result; unsigned tries=0;
        if (!gui_media_start() || !gui_media_identify(argv[2],atoi(argv[3]),91,0)) return 2;
        while (!gui_media_result(&result) && ++tries<10000) SDL_Delay(1);
        gui_media_stop();
        if (tries>=10000) return 3;
        printf("%u\t%u\t%d\t%s\t%s\n",result.disc,result.generation,result.state,result.path,result.detail);
        return 0;
    }
    if (!strcmp(argv[1],"scan") && argc==4) {
        gui_disc_result_t result; unsigned wanted=(unsigned)atoi(argv[3]),got=0,tries=0;
        if (!gui_media_start() || !gui_media_scan(argv[2],37)) return 2;
        while (got<wanted && ++tries<10000) {
            if (!gui_media_result(&result)) { SDL_Delay(1); continue; }
            printf("%u\t%u\t%d\t%s\t%s\n",result.disc,result.generation,result.state,result.path,result.detail); ++got;
        }
        gui_media_stop(); return got==wanted ? 0 : 3;
    }
    if (!strcmp(argv[1],"string") && argc==3) { puts(host_gui_string(argv[2])); return 0; }
    return 64;
}
