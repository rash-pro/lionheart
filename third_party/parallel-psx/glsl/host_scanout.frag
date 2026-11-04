#version 450
// Host scanout uses integer texel coordinates, including PS1 row/byte wrapping.
layout(set=0, binding=0) uniform sampler2D scaled_vram;
layout(set=0, binding=1) uniform usampler2D native_vram;
layout(push_constant, std430) uniform Scanout { uvec4 origin_scale_flags; } scanout;
layout(location=0) out vec4 color;
void main()
{
    uvec4 r=scanout.origin_scale_flags;
    if ((r.w & 4u)==0u) { color=vec4(0,0,0,1); return; }
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    if ((r.w & 1u)!=0u) {
        int row=(int(r.y)+pixel.y/int(r.z)) & 511;
        int at=int(r.x)*2+(pixel.x/int(r.z))*3;
        uvec3 rgb;
        for (int k=0;k<3;++k) {
            int byte_at=(at+k)&2047;
            uint value=texelFetch(native_vram,ivec2(byte_at/2,row),0).r;
            rgb[k]=(value >> uint((byte_at&1)*8)) & 255u;
        }
        int word_x=(at&2047)/2;
        uint marked=texelFetch(native_vram,ivec2(word_x,row),0).r;
        if ((marked&65536u)!=0u) {
            ivec2 coord=ivec2((int(r.x*r.z)+pixel.x*3/2) % textureSize(scaled_vram,0).x,
                             (int(r.y*r.z)+pixel.y) % textureSize(scaled_vram,0).y);
            color=vec4(texelFetch(scaled_vram,coord,0).rgb,1);
        } else color=vec4(vec3(rgb)/255.0,1);
    } else {
        ivec2 coord=(ivec2(r.xy*r.z)+pixel) % textureSize(scaled_vram,0);
        vec3 rgb=texelFetch(scaled_vram,coord,0).rgb;
        if ((r.w & 2u)==0u) {
            uvec3 v=uvec3(round(rgb*255.0));
            rgb=vec3((v&248u)|(v>>5u))/255.0;
        }
        color=vec4(rgb,1);
    }
}
