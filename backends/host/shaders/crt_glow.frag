#version 450
// Pass 3/4: separable Gaussian for bloom and halation at reduced resolution.
// Pairs of source texels are read through the linear sampler, so each tap
// covers two texels along the blur direction.
#include "crt_common.h"
layout(set=0, binding=0) uniform sampler2D source;
layout(push_constant, std430) uniform Glow {
    vec4 size;  // source width, source height, output width, output height
    vec4 blur;  // direction x, direction y (texels per pair), sigma (pairs), power
} glow;
layout(location=0) out vec4 color;
void main()
{
    vec2 texel=1.0/glow.size.xy;
    vec2 center=gl_FragCoord.xy*glow.size.xy/glow.size.zw;
    int radius=int(ceil(3.0*glow.blur.z));
    vec3 acc=vec3(0.0);
    float sum=0.0;
    for (int k=-radius;k<=radius;++k) {
        float w=gauss(float(k),glow.blur.z);
        vec3 c=texture(source,(center+glow.blur.xy*float(k))*texel).rgb;
        if (glow.blur.w!=1.0) c=pow(max(c,0.0),vec3(glow.blur.w));
        acc+=c*w; sum+=w;
    }
    color=vec4(acc/sum,1.0);
}
