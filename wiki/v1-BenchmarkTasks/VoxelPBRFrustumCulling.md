# VoxelPBRFrustumCulling

v1 中最复杂的任务：在保持像素风格的同时正确实现 PBR、实例化绘制与可观察、可验证的 CPU 视锥剔除。

## 题目介绍

渲染一个类 Minecraft 的像素方块场景（严格 `250` 个方块：`225` 个地面方块 + `5` 根柱子），使用 C++、GLFW、GLAD、GLM 与 nlohmann-json，以 OpenGL 4.6 core profile 为目标：

- Cook-Torrance metallic-roughness PBR 与线性颜色工作流；`brushed_metal` / `red_plastic` 两种固定材质由 `materials.json` 经 nlohmann-json 解析并校验后作为唯一真源
- 严格 `32 x 32` 的程序化材质纹理（base-color / normal / ORM）；主场景先渲染到 `640 x 360` 离屏目标，再以 `GL_NEAREST` 放大到窗口
- 程序化 IBL：HDR environment cubemap、irradiance、5-mip prefilter 与 BRDF LUT（split-sum approximation）
- directional sun shadow map（≥ `2048 x 2048`、3×3 PCF）+ 4 个 point light；Reinhard/ACES tone mapping 与 gamma correction
- 实例化绘制（主场景每帧 cube draw call ≤ `2` 次）、CPU 视锥剔除（`C` 开关，可见实例数可降至 `0`）、标题栏实时统计、5 种 debug view
- 所有 GLSL shader 必须作为独立 UTF-8 文本文件由程序读取，禁止嵌入 C++ 源码

完整任务定义：[`VoxelPBRFrustumCulling_TASK.md`](https://github.com/yhcedpn/RenderArena/blob/main/OpenGL/VoxelPBRFrustumCulling_Codex+gpt-5.6-terra@max/VoxelPBRFrustumCulling_TASK.md)

## 相关评审 issue

- [gpt-5.6-luna 执行 VoxelPBRFrustumCulling 任务（#21）](https://github.com/yhcedpn/RenderArena/issues/21)
- [gpt-5.6-terra 执行 VoxelPBRFrustumCulling 任务（#17）](https://github.com/yhcedpn/RenderArena/issues/17)
