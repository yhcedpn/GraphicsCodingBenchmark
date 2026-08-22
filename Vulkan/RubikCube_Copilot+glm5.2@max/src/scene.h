#pragma once
// 场景布局：部分缺失三阶魔方 + 9x9 金属地板。
// 立方体几何、实例列表、相机/投影、UBO 数据。
// 材质索引引用 Config 中的材质（由 id 解析），不在代码硬编码材质数值。

#include "config.h"
#include <cstdint>
#include <vector>
#include <array>

namespace rubik {

// 立方体顶点（位置/法线/切线/uv），24 顶点（每面 4 个，便于面法线与切线）
struct Vertex {
    float pos[3];
    float normal[3];
    float tangent[3]; // 切线（用于法线贴图 TBN，与 UV 方向一致）
    float uv[2];
};

// 每实例：模型矩阵（mat4 列主序，16 float）+ 材质索引
struct Instance {
    float model[16]; // 列主序 4x4 模型矩阵（本场景为平移矩阵）
    uint32_t matIdx;
};

// 相机 UBO（与 shader set0 binding0 对应）
struct CameraUbo {
    float viewProj[16]; // 列主序
    float camPos[3];
    float _pad; // 对齐到 vec4
};

// 场景固定光照（非材质数据）
struct alignas(16) LightUbo {
    float lightDir[3];
    float lightIntensity;
    float lightColor[3];
    float ambientStrength;
};

// 场景材质参数（与 shader MaterialParams 对应，通过 push constant 传入）
struct MaterialParams {
    float baseColorLinearSRGBA[4]; // xyz 线性基础色, w = ambientOcclusion
    float metallicRoughnessNormStrength[4]; // x metallic, y roughness, z normalStrength
};

// 生成单位立方体顶点 + 索引（24 顶点 / 36 索引）
void makeCubeGeometry(std::vector<Vertex>& verts, std::vector<uint32_t>& indices);

// 按场景规格生成实例列表。
// matBottomLayer/matMidLayer/matTopLayer/matFloor 为材质索引（由 Config::indexOf 解析）。
std::vector<Instance> buildSceneInstances(int matBottomLayer, int matMidLayer,
                                          int matTopLayer, int matFloor);

// 相机：透视投影，初始位置 (5,4,6) 看向世界原点 (0,0,0)，fov 60°
// 支持交互式轨道相机：围绕 target 旋转、缩放距离、平移目标。
struct Camera {
    float pos[3] = {5.0f, 4.0f, 6.0f};
    float target[3] = {0.0f, 0.0f, 0.0f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    float fovDeg = 60.0f;
    float aspect = 1.0f;
    float nearZ = 0.1f;
    float farZ = 100.0f;
    // 轨道相机控制状态（球面坐标，由输入更新，pos 由其推导）
    float yaw = 0.0f;      // 方位角（绕 Y），弧度
    float pitch = 0.0f;    // 仰角，弧度
    float distance = 0.0f; // 到 target 的距离
    bool  orbitInit = false; // 是否已用初始 pos/target 初始化球面坐标
};

// 根据球面坐标（yaw/pitch/distance/target）更新 camera.pos
void updateCameraOrbit(Camera& cam);

// 计算 viewProj（列主序）与 camPos，填充 CameraUbo
CameraUbo buildCameraUbo(const Camera& cam, uint32_t fbW, uint32_t fbH);

// sRGB -> 线性
float srgbToLinear(float c);
// 由 Material 生成 shader 用 MaterialParams（基础色 sRGB->线性）
MaterialParams buildMaterialParams(const Material& m);

} // namespace rubik
