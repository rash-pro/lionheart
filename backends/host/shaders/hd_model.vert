#version 450

// Clean-room MIT HD 3D model vertex shader for Lionheart (Phase 4).
// Performs up to 4-bone linear-blend skinning with live float32 PS1 GTE bone
// matrices and projects directly into parallel-psx's scaled VRAM plane with
// true perspective-correct interpolation and hardware Z-buffer depth.

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

layout(set = 0, binding = 1, std140) uniform HdModelUniforms {
    vec4 local_row0;     // model local affine transform row 0 (xyz = rot*scale, w = offset.x)
    vec4 local_row1;     // row 1 (w = offset.y)
    vec4 local_row2;     // row 2 (w = offset.z)
    vec4 proj_params;    // x = draw_ox + ofx, y = draw_oy + ofy, z = h, w = float(bone_count)
    vec4 tint_params;    // xyz = captured PS1 GTE lighting tint (1.0 = neutral 128), w = alpha_cutoff
    vec4 base_color;     // glTF material baseColorFactor RGBA
    uvec4 flags;         // x = alpha_mode (0=opaque, 1=mask, 2=blend), y = has_texture, z/w = reserved
    vec4 bone_rows[384]; // up to 128 GTE bones x 3 rows (xyz = rotation row, w = translation)
} u;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_normal;

void main() {
    vec3 local_pos = vec3(
        dot(u.local_row0.xyz, in_pos) + u.local_row0.w,
        dot(u.local_row1.xyz, in_pos) + u.local_row1.w,
        dot(u.local_row2.xyz, in_pos) + u.local_row2.w
    );
    vec3 local_nrm = vec3(
        dot(u.local_row0.xyz, in_normal),
        dot(u.local_row1.xyz, in_normal),
        dot(u.local_row2.xyz, in_normal)
    );

    uint max_bone = max(1u, uint(u.proj_params.w)) - 1u;
    vec3 view_pos = vec3(0.0);
    vec3 view_nrm = vec3(0.0);
    float total_w = 0.0;

    for (int i = 0; i < 4; ++i) {
        float w = in_weights[i];
        if (w <= 0.0) continue;
        uint bi = min(in_joints[i], max_bone);
        uint base = bi * 3u;
        vec4 r0 = u.bone_rows[base + 0u];
        vec4 r1 = u.bone_rows[base + 1u];
        vec4 r2 = u.bone_rows[base + 2u];
        view_pos += w * vec3(
            dot(r0.xyz, local_pos) + r0.w,
            dot(r1.xyz, local_pos) + r1.w,
            dot(r2.xyz, local_pos) + r2.w
        );
        view_nrm += w * vec3(
            dot(r0.xyz, local_nrm),
            dot(r1.xyz, local_nrm),
            dot(r2.xyz, local_nrm)
        );
        total_w += w;
    }

    if (total_w <= 1e-6) {
        vec4 r0 = u.bone_rows[0u];
        vec4 r1 = u.bone_rows[1u];
        vec4 r2 = u.bone_rows[2u];
        view_pos = vec3(
            dot(r0.xyz, local_pos) + r0.w,
            dot(r1.xyz, local_pos) + r1.w,
            dot(r2.xyz, local_pos) + r2.w
        );
        view_nrm = vec3(
            dot(r0.xyz, local_nrm),
            dot(r1.xyz, local_nrm),
            dot(r2.xyz, local_nrm)
        );
    }

    float sz = max(1.0, view_pos.z);
    float inv = u.proj_params.z / sz;
    float vram_x = u.proj_params.x + view_pos.x * inv;
    float vram_y = u.proj_params.y + view_pos.y * inv;

    // Map PS1 VRAM [0..1024, 0..512] to Vulkan NDC [-1..1, -1..1].
    float ndc_x = (vram_x / 512.0) - 1.0;
    float ndc_y = (vram_y / 256.0) - 1.0;

    // Projective depth mapping [z_near..z_far] -> [0..1] with w = sz for
    // perspective-correct texture & normal interpolation and monotonic hardware Z.
    const float z_near = 8.0;
    const float z_far = 65536.0;
    float sz_c = clamp(sz, z_near, z_far);
    float z_a = z_far / (z_far - z_near);
    float z_b = -(z_near * z_far) / (z_far - z_near);
    float z_ndc = clamp(z_a + z_b / sz_c, 0.0, 1.0);

    gl_Position = vec4(ndc_x * sz, ndc_y * sz, z_ndc * sz, sz);
    v_uv = in_uv;
    float nlen = length(view_nrm);
    v_normal = (nlen > 1e-6) ? (view_nrm / nlen) : vec3(0.0, -1.0, 0.0);
}
