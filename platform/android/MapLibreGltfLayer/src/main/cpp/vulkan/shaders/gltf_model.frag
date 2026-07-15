#version 450

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 matrix;
    vec4 baseColor;
    float alphaCutoff;
    float hasTexture;
    float darkModeLighting;
} pc;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

void main() {
    vec4 color = pc.baseColor;
    if (pc.hasTexture > 0.5) {
        color *= texture(u_texture, v_uv);
    }
    if (pc.alphaCutoff > 0.0 && color.a < pc.alphaCutoff) {
        discard;
    }
    if (pc.darkModeLighting < 0.5) {
        fragColor = color;
        return;
    }
    vec3 n = normalize(v_normal);
    vec3 light = normalize(vec3(0.4, 0.3, 0.85));
    float lambert = 0.45 + 0.55 * max(dot(n, light), 0.0);
    fragColor = vec4(color.rgb * lambert, color.a);
}
