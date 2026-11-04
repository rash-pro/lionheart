#version 450
// Pass 2: horizontal video bandwidth. Resamples each native line to the output
// width with a Gaussian in source texels; the composite path filters luma and
// chroma separately and delays chroma, which gives the colour bleed of a TV.
#include "crt_common.h"
layout(set=0, binding=0) uniform sampler2D lines;
layout(push_constant, std430) uniform Beam {
    vec4 size;   // source width, line count, output width, unused
    vec4 filt;   // luma sigma (texels), chroma sigma (texels), composite flag, chroma delay (texels)
} beam;
layout(location=0) out vec4 color;
const mat3 RGB_TO_YIQ=mat3(0.299,0.596,0.211, 0.587,-0.274,-0.523, 0.114,-0.322,0.312);
const mat3 YIQ_TO_RGB=mat3(1.0,1.0,1.0, 0.956,-0.272,-1.106, 0.621,-0.647,1.703);
void main()
{
    float source_w=beam.size.x;
    vec2 texel=vec2(1.0/source_w,1.0/beam.size.y);
    float row=(floor(gl_FragCoord.y)+0.5)*texel.y;
    float sx=gl_FragCoord.x*source_w/beam.size.z; // source position in texels
    float center=floor(sx)+0.5;
    float sigma_y=max(beam.filt.x,0.05), sigma_c=max(beam.filt.y,sigma_y);
    bool composite=beam.filt.z>0.5;
    int radius=int(ceil(3.0*(composite ? sigma_c : sigma_y)));
    vec3 acc_y=vec3(0.0), acc_c=vec3(0.0);
    float sum_y=0.0, sum_c=0.0;
    for (int k=-radius;k<=radius;++k) {
        float x=center+float(k);
        vec3 c=texture(lines,vec2(x*texel.x,row)).rgb;
        float wy=gauss(x-sx,sigma_y);
        acc_y+=c*wy; sum_y+=wy;
        if (composite) {
            float wc=gauss(x-sx-beam.filt.w,sigma_c);
            acc_c+=c*wc; sum_c+=wc;
        }
    }
    vec3 rgb=acc_y/sum_y;
    if (composite) {
        vec3 yiq=RGB_TO_YIQ*rgb, chroma=RGB_TO_YIQ*(acc_c/sum_c);
        rgb=max(YIQ_TO_RGB*vec3(yiq.x,chroma.yz),0.0);
    }
    color=vec4(rgb,1.0);
}
