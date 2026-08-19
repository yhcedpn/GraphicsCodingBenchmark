# CachedCubePipelines

中等难度的任务：用标准 OpenGL 4.6 实现“不依赖厂商扩展的命令/状态缓存绘制”，考察命令/状态缓存与双 pipeline 重放的代码组织能力。

## 题目介绍

在 CPU 侧缓存绘制指令和渲染状态，然后每帧通过重放缓存指令，用两个不同的渲染 pipeline 绘制一个由多个 cube 组成的 3D 场景（不少于 `48` 个 cube）。

- `1280 x 720` 窗口，标题 `OpenGL CachedCubePipelines Test`，OpenGL 4.6 core profile
- Pipeline A：带法线光照（diffuse/specular、Blinn-Phong）的实体 cube；Pipeline B：另一种视觉风格（线框、法线可视化、描边等）
- 初始化阶段构建 CPU 侧缓存指令列表（shader/pipeline id、VAO、index count、transform、material、depth/cull/wireframe 等状态），每帧重放，不重建静态资源
- 按 pipeline/渲染状态分组排序，尽量避免重复调用 `glUseProgram`、重复绑定 VAO 或切换相同状态
- 使用 UBO 传递共享帧数据；一个简单确定性的动画；`WASD` 相机、`Space` 暂停/恢复、`1`/`2` 切换两个 pipeline 分组
- 禁止使用 `GL_NV_command_list`、bindless graphics 或任何厂商专属 OpenGL 扩展

完整任务定义：[`CachedCubePipelines_TASK.md`](https://github.com/yhcedpn/RenderArena/blob/main/OpenGL/CachedCubePipelines_Codex+gpt-5.5@xhigh/CachedCubePipelines_TASK.md)

## 相关评审 issue

- [gpt-5.4 执行 CachedCubePipelines 任务（#2）](https://github.com/yhcedpn/RenderArena/issues/2)
- [gpt-5.5 执行 CachedCubePipelines 任务（#3）](https://github.com/yhcedpn/RenderArena/issues/3)
