#version 450

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

layout(push_constant) uniform PushConstants {
    mat4 matrix;
    vec4 baseColor;
    float alphaCutoff;
    float hasTexture;
    float darkModeLighting;
} pc;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;

void main() {
    gl_Position = pc.matrix * vec4(a_pos, 1.0);
    gl_Position.y *= -1.0;
    v_normal = a_normal;
    v_uv = a_uv;
}
