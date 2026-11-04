#version 450
// Pass 5: composite at output resolution. Beams follow the native scanlines
// (area-sampled Gaussian profiles whose width grows with brightness), the
// phosphor mask lives on output pixels, glow/halation is added unmasked, then
// tube curvature, rounded corners and vignette shape the glass.
#include "crt_common.h"
layout(set=0, binding=0) uniform sampler2D beam;
layout(set=0, binding=1) uniform sampler2D glow;
layout(push_constant, std430) uniform Final {
    vec4 content;  // scanout rectangle in output pixels: x, y, w, h
    vec4 frame;    // TV aperture in output pixels: x, y, w, h
    vec4 geom;     // native lines, glow strength, convergence (pixels), saturation
    vec4 profile;  // beam sigma dark, beam sigma bright (line pitch units), width power, gain
    vec4 mask;     // type (0 none, 1 grille, 2 dots, 3 slots), strength, bright fade, period (pixels)
    vec4 shape;    // curvature x, curvature y, corner radius, vignette
    vec4 limits;   // allowed peak over 1.0 before the beam blooms wider, glow power, unused
} fin;
float mask_gain(float level)
{
    float strength=fin.mask.y*(1.0-fin.mask.z*clamp(level,0.0,1.0));
    return fin.mask.x>0.0 ? 1.0/(1.0-strength*(1.0-fin.profile.w)) : 1.0;
}
layout(location=0) out vec4 color;
// Abramowitz-Stegun 7.1.26; integrates the beam profile over each output pixel.
vec3 erf3(vec3 x)
{
    vec3 s=sign(x); x=abs(x);
    vec3 t=1.0/(1.0+0.3275911*x);
    vec3 poly=((((1.061405429*t-1.453152027)*t+1.421413741)*t-0.284496736)*t+0.254829592)*t;
    return s*(1.0-poly*exp(-x*x));
}
void main()
{
    vec2 p=gl_FragCoord.xy;
    vec2 fc=(p-fin.frame.xy)/fin.frame.zw*2.0-1.0;
    vec2 tc=fc*(1.0+vec2(fin.shape.x*fc.y*fc.y,fin.shape.y*fc.x*fc.x));
    float corner=fin.shape.z;
    vec2 d=abs(tc)-(1.0-corner);
    float dist=length(max(d,0.0))+min(max(d.x,d.y),0.0)-corner;
    float edge=max(fwidth(dist),1e-4);
    float border=1.0-smoothstep(-edge,edge,dist);
    if (border<=0.0) { color=vec4(0.0,0.0,0.0,1.0); return; }
    vec2 tp=fin.frame.xy+(tc*0.5+0.5)*fin.frame.zw;
    float lines=fin.geom.x, pitch=fin.content.w/lines;
    float u=(tp.x-fin.content.x)/fin.content.z;
    float ly=(tp.y-fin.content.y)/pitch;
    float conv=fin.geom.z/fin.content.z;
    vec3 acc=vec3(0.0), local=vec3(0.0);
    if (u>=0.0 && u<1.0 && ly>=0.0 && ly<lines) { // beams stay inside the scanout rectangle, like the blit
        int first=int(floor(ly))-1;
        for (int i=first;i<=first+2;++i) {
            if (i<0 || i>=int(lines)) continue;
            float v=(float(i)+0.5)/lines;
            vec3 c=vec3(texture(beam,vec2(u-conv,v)).r,texture(beam,vec2(u,v)).g,texture(beam,vec2(u+conv,v)).b);
            if (i==first+1) local=c;
            vec3 sigma=mix(vec3(fin.profile.x),vec3(fin.profile.y),pow(clamp(c,0.0,1.0),vec3(fin.profile.z)));
            // A beam carries its line's energy; when the peak would clip it blooms wider instead.
            sigma=max(sigma,0.39894228*c*mask_gain(luma(c))/fin.limits.x)*pitch;
            float dy=tp.y-(fin.content.y+(float(i)+0.5)*pitch);
            vec3 w=0.5*(erf3((dy+0.5)/(sigma*1.41421356))-erf3((dy-0.5)/(sigma*1.41421356)));
            acc+=c*w*pitch; // each line carries pitch units: a flat field averages back to c
        }
    }
    acc=mix(vec3(luma(acc)),acc,fin.geom.w);
    vec3 m=vec3(1.0);
    float gain=1.0;
    int type=int(fin.mask.x);
    if (type>0) {
        float period=fin.mask.w, x=floor(p.x), y=floor(p.y);
        float shift=type==2 ? mod(floor(y/period),2.0)*period*0.5 : 0.0;
        int lit=int(floor(mod(x+shift,period)*3.0/period+1e-3)); // 0 red, 1 green, 2 blue
        m=vec3(lit==0 ? 1.0 : 0.0,lit==1 ? 1.0 : 0.0,lit==2 ? 1.0 : 0.0);
        if (type==3) {
            float slot=2.0*period, column=floor(x/period);
            float yp=y+(mod(column,2.0)<1.0 ? 0.0 : slot*0.5);
            if (mod(yp,slot)<period/3.0) m=vec3(0.0);
        }
        float strength=fin.mask.y*(1.0-fin.mask.z*clamp(luma(acc),0.0,1.0));
        m=mix(vec3(1.0),m,strength);
        gain=mask_gain(luma(acc)); // keeps the mean level over the mask's average transmission
    }
    // Glow redistributes light: the blurred term is added and the same amount
    // is taken from the beam, so a flat field keeps its level while bright
    // areas spill into dark ones and fill their own scanline gaps.
    vec3 c=acc*m*gain*(1.0-fin.geom.y*pow(clamp(local,0.0,1.0),vec3(fin.limits.y-1.0)));
    vec2 gv=(tp-fin.content.xy)/fin.content.zw;
    if (gv.x>=0.0 && gv.x<1.0 && gv.y>=0.0 && gv.y<1.0) c+=texture(glow,gv).rgb*fin.geom.y;
    c*=border*(1.0-fin.shape.w*dot(fc,fc)*0.5);
    float noise=fract(sin(dot(p,vec2(12.9898,78.233)))*43758.5453);
    color=vec4(linear_to_srgb(c)+(noise-0.5)/255.0,1.0);
}
