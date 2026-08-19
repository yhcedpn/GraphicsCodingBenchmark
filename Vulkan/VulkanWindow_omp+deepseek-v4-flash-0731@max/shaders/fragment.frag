#version 450
// 四象限纯色：分界线位于水平与垂直中心。
// Vulkan 中 gl_FragCoord 原点在左上角，y 向下增长。
// 左上=红，左下=白，右下=蓝，右上=绿。
layout(push_constant) uniform PushConstants {
    vec2 uResolution;  // 当前帧缓冲尺寸（像素）
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    bool top = gl_FragCoord.y < pc.uResolution.y * 0.5;
    bool left = gl_FragCoord.x < pc.uResolution.x * 0.5;
    if (left) {
        outColor = top ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(1.0, 1.0, 1.0, 1.0);
    } else {
        outColor = top ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(0.0, 0.0, 1.0, 1.0);
    }
}
