#version 450
// Original host OSD. UNORM attachments blend the panel in sRGB code values,
// just like SDL_BLENDMODE_BLEND. The viewport covers exactly the panel pixels.
layout(set=0,binding=0) uniform sampler2D panel;
layout(push_constant) uniform Push { vec4 rect; float opacity; } pc;
layout(location=0) out vec4 color;
void main() {
    color=texelFetch(panel,ivec2(gl_FragCoord.xy-pc.rect.xy),0);
    color.a=floor(color.a*255.0*pc.opacity)/255.0;
}
