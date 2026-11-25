#version 450

// Clean-room MIT HD 3D model fragment shader for Lionheart (Phase 4).
// Supports opaque, alpha-masked (hair/edges), and alpha-blended materials,
// modulated by live captured PS1 GTE vertex lighting/tint.

layout(set = 0, binding = 0) uniform sampler2D base_tex;

layout(set = 0, binding = 1, std140) uniform HdModelUniforms {
    vec4 local_row0;
    vec4 local_row1;
    vec4 local_row2;
    vec4 proj_params;
    vec4 tint_params;
    vec4 base_color;
    uvec4 flags;
    vec4 bone_rows[384];
} u;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_normal;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex = (u.flags.y != 0u) ? texture(base_tex, v_uv) : vec4(1.0);
    vec4 albedo = tex * u.base_color;

    // Alpha-test (MASK mode: hair strands, clothing edges)
    if (u.flags.x == 1u && albedo.a < u.tint_params.w)
        discard;

    // Gentle view-space directional fill modulated by live PS1 GTE vertex tint
    // (1.0 = PS1 neutral 128; fades smoothly to black on scene transitions and
    // matches field shadows, battle lighting, and damage/status flashes).
    float nlen = length(v_normal);
    vec3 n = (nlen > 1e-6) ? (v_normal / nlen) : vec3(0.0, -1.0, 0.0);
    vec3 light_dir = normalize(vec3(-0.35, -0.55, -0.75));
    float ndl = max(dot(n, light_dir), 0.0);
    float shade = 0.74 + 0.26 * ndl;

    vec3 lit = clamp(albedo.rgb * u.tint_params.rgb * shade, 0.0, 1.0);

    // For opaque/mask draws, output alpha = 0.0 so PS1 VRAM mask bit stays clear.
    // For alpha-blended draws, output material alpha (pipeline preserves dst alpha).
    float out_a = (u.flags.x == 2u) ? albedo.a : 0.0;
    out_color = vec4(lit, out_a);
}
