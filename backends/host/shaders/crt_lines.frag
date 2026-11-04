#version 450
// Pass 1: collapse the scaled scanout into one linear-light row per native
// scanline. The internal scale's sub-rows are averaged, so upscaled geometry
// keeps its detail inside each beam while the beam count stays native.
#include "crt_common.h"
layout(set=0, binding=0) uniform sampler2D scanout;
layout(push_constant, std430) uniform Lines { uvec4 scale_lines; } lines;
layout(location=0) out vec4 color;
void main()
{
    ivec2 at=ivec2(gl_FragCoord.xy);
    int scale=int(lines.scale_lines.x);
    vec3 sum=vec3(0.0);
    for (int row=0;row<scale;++row)
        sum+=srgb_to_linear(texelFetch(scanout,ivec2(at.x,at.y*scale+row),0).rgb);
    color=vec4(sum/float(scale),1.0);
}
