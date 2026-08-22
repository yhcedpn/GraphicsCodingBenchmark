#version 460
#extension GL_EXT_scalar_block_layout : enable
#extension GL_EXT_nonuniform_qualifier : enable

// 场景顶点着色器：立方体几何 + 每实例模型矩阵/材质索引（push constant）
// 使用动态渲染，无 RenderPass/Framebuffer。

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inTangent;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec3 fragTangent;
layout(location = 3) out vec2 fragTexCoord;
layout(location = 4) flat out uint fragMatIdx;

// 每实例：模型矩阵（mat4）+ 材质索引（push constant）
layout(push_constant) uniform PushConstants {
    mat4 model;
    uint matIdx;
    // matParams 在片元阶段使用，顶点阶段不引用；两阶段共享同一 push constant 块
} pc;

layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    vec3 camPos;
} camera;

out gl_PerVertex {
    vec4 gl_Position;
};

void main() {
    vec4 world = pc.model * vec4(inPosition, 1.0);
    fragWorldPos = world.xyz;
    // 法线/切线：本场景模型矩阵仅含平移，左上 3x3 为单位阵
    fragNormal = mat3(pc.model) * inNormal;
    fragTangent = mat3(pc.model) * inTangent;
    fragTexCoord = inTexCoord;
    fragMatIdx = pc.matIdx;
    gl_Position = camera.viewProj * world;
}
