#include <stdio.h>
#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
int main(void) {
    SDL_version version; SDL_GetVersion(&version);
    printf("SDL %u.%u.%u\n",version.major,version.minor,version.patch);
    printf("avcodec: %s\navformat: %s\navutil: %s\nswscale: %s\n",
           avcodec_license(),avformat_license(),avutil_license(),swscale_license());
    printf("configure: %s\n",avcodec_configuration());
    return 0;
}
