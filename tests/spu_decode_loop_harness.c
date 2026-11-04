#define FF8_PLAY
#define FF8_SPU_CACHE_TEST
#include "../runtime/spu.c"
#include <assert.h>
#include <stdio.h>
static void irq_callback(void *context) { (void)context; }
int main(void) {
    static const spu_backend_t sink={NULL,NULL};
    static const unsigned steps[]={0,1,0x0fff,0x1000,0x3fff,0xffff};
    static const unsigned counts[]={1,7,28,64,183,4096};
    unsigned predictor,pitch,flags,mode,j,k;uint64_t skipped=0;
    for(predictor=0;predictor<5;++predictor)for(pitch=0;pitch<6;++pitch)
    for(flags=3;flags<=7;flags+=4)for(mode=0;mode<6;++mode) {
        spu_t *a=spu_create(),*b=malloc(sizeof(*b));spu_control_t replay={0};
        assert(a&&b);a->backend=&sink;a->replay=1;
        REG(a,0x1aa)=0xc040;REG(a,0x1a4)=0x400;
        REG(a,4)=steps[pitch];REG(a,6)=REG(a,14)=0x400;
        REG(a,8)=0xf;REG(a,10)=0x1fc0;
        memset(a->ram+0x2000,predictor?0:0x77,16);
        a->ram[0x2000]=predictor?(uint8_t)(predictor<<4):0x09;a->ram[0x2001]=(uint8_t)flags;
        a->voices[0].address=0x2000;a->voices[0].position=mode==2?25:0;
        a->voices[0].fraction=mode==2?4095:0;
        if(mode==2){a->voices[0].history[0]=1234;a->voices[0].history[1]=-321;}
        if(mode==3){
            static const int16_t fixed[]={1,16,64,3,32};
            memset(a->ram+0x2000,0x11,16);
            a->ram[0x2000]=(uint8_t)((predictor<<4)|12);a->ram[0x2001]=(uint8_t)flags;
            a->voices[0].history[0]=a->voices[0].history[1]=fixed[predictor];
        }
        voice_decode(a,0);
        assert(!voice_decode_loop(a,0,4097,0x1000));
        {
            uint16_t status=a->status;
            a->irq=irq_callback;a->status&=~0x40u;
            assert(!voice_decode_loop(a,0,28,0x1000));
            a->status=status;a->irq=NULL;
        }
        if(mode==1){for(k=3;k<31;++k)a->voices[0].samples[k]=(int16_t)(k*313);REG(a,14)=0x401;}
        if(mode>=4) {
            REG(a,0x1aa)=mode==4?0xc000:0xc040;
            a->status=mode==5?0x40:0;a->irq=irq_callback;
        }
        memcpy(b,a,sizeof(*b));replay.shadow=b;
        if(mode>=4){b->replay=0;b->control=&replay;}
        for(j=0;j<6;++j) {
            uint64_t until=a->next_sample+(counts[j]-1)*768;
            if(mode==2&&j==2){a->ram[0x2002]^=0x37;b->ram[0x2002]=a->ram[0x2002];}
            while(a->next_sample<=until){sample(a);a->next_sample+=768;}
            control_replay(&replay,until);
            assert(!memcmp(a,b,offsetof(spu_t,output)));
            assert(!memcmp(a->output,b->output,sizeof(a->output)));
            assert(a->next_sample==b->next_sample&&a->frames==b->frames&&a->blocks==b->blocks);
        }
        if(mode==3 && steps[pitch]>=0x0fff && steps[pitch]<=0x4000)assert(b->decode_skipped);
        skipped+=b->decode_skipped;b->control=NULL;spu_destroy(a);spu_destroy(b);
    }
    assert(skipped>1000);printf("PASS fixed decoder loops skipped=%llu; scalar full RAM/state/CD output, pitch limits, flags, old interpolation tails, IRQ status and RAM mutation\n",(unsigned long long)skipped);return 0;
}
