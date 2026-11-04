// Shared helpers for the CRT presentation passes. All passes work in linear light.
vec3 srgb_to_linear(vec3 c)
{
    vec3 low=c/12.92, high=pow((c+0.055)/1.055,vec3(2.4));
    return mix(high,low,lessThanEqual(c,vec3(0.04045)));
}
vec3 linear_to_srgb(vec3 c)
{
    c=clamp(c,0.0,1.0);
    vec3 low=c*12.92, high=1.055*pow(c,vec3(1.0/2.4))-0.055;
    return mix(high,low,lessThanEqual(c,vec3(0.0031308)));
}
float luma(vec3 c) { return dot(c,vec3(0.2126,0.7152,0.0722)); }
float gauss(float x,float sigma) { return exp(-0.5*x*x/(sigma*sigma)); }
