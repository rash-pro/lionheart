#include "disc_id.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#ifdef FF8_DISC_RUNTIME_TEST
struct hw *hw_get(cpu_t *c) { return c->hw; }
#ifdef FF8_DISC_ALLOC_TEST
void *__real_malloc(size_t);
void *__wrap_malloc(size_t n) {
    return getenv("FF8_TEST_FAIL_ALLOC") ? NULL : __real_malloc(n);
}
#endif
static int runtime_open(const char *input, char *error, size_t error_size) {
    host_disc_runtime_t prepared;
    hw_cd_t expected;
    static cpu_t cpu;
    static struct hw hw;
    int ok = 0;
    if (!host_disc_open(input,&expected,error,error_size)) return 0;
    if (!host_disc_runtime_path(input,&prepared,error,error_size)) { fclose(expected.file); return 0; }
    cpu.hw = &hw;
    if (cdrom_open(&cpu,prepared.path,error,error_size)) {
        ok = hw.cd.offset == expected.offset && hw.cd.sectors == expected.sectors;
        if (ok) printf("runtime offset=%u sectors=%u temporary=%d\n",hw.cd.offset,hw.cd.sectors,prepared.temporary);
        else snprintf(error,error_size,"runtime disc geometry changed");
        fclose(hw.cd.file); hw.cd.file = NULL;
        free(hw.cd.lba_reads); free(hw.cd.lba_dma);
    }
    fclose(expected.file);
    host_disc_runtime_cleanup(&prepared);
    return ok;
}
#endif
int main(int argc, char **argv) {
    char error[512]; const char *slots[4] = {0}; unsigned n, i;
    if (argc < 3) return 2;
#ifdef FF8_DISC_RUNTIME_TEST
    if (!strcmp(argv[1],"runtime")) {
        if (!runtime_open(argv[2],error,sizeof(error))) goto fail;
    } else
#endif
    if (!strcmp(argv[1],"assign")) {
        for (i=2;i<(unsigned)argc;++i) if (!host_disc_assign(slots,argv[i],0,error,sizeof(error))) goto fail;
        for (i=0;i<4;++i) printf("%u=%s\n",i+1,slots[i] ? slots[i] : "");
    } else if (!strcmp(argv[1],"exe")) {
        uint8_t *data; size_t size;
        if (!host_disc_exe(argv[2],&data,&size,error,sizeof(error))) goto fail;
        i = host_exe_check(data,size,error,sizeof(error)); free(data);
        if (!i) goto fail;
        puts("EXE verified");
    } else {
        if (!host_disc_identify(argv[2],!strcmp(argv[1],"full"),&n,error,sizeof(error))) goto fail;
        printf("disc=%u\n",n);
    }
    return 0;
fail:
    fprintf(stderr,"%s\n",error); return 2;
}
