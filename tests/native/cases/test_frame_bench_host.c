#include <stdio.h>
#include <stdlib.h>
int main(void) {
    FILE *f=fopen(getenv("FF8_FRAME_TIMINGS"),"w");if(!f)return 2;
    fputs("vblank,wall_ms,cpu_ms,cycles,interpreted,compiled,idle,gte,cd_bytes\n",f);
    int checkpoint=getenv("CHECKPOINT")!=NULL;
    for(int n=checkpoint?5:1;n<=(checkpoint?6:getenv("INCOMPLETE")?1:2);++n)fprintf(f,"%d,2,1,10,5,0,0,0,0\n",n);
    fclose(f);printf("Stop: VBlank limit reached\nHardware: cycles=%d VBlanks=%d \n",checkpoint?60:20,checkpoint?6:2);return 0;
}
