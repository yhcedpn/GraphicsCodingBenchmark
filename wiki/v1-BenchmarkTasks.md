# v1 —— OpenGL 图形编程任务

v1 对应当前仓库 `OpenGL/` 目录下的四道 OpenGL 4.6 Core Profile 图形编程任务（难度由简单到复杂），以及不同模型在这些任务上的完成结果与评审。v1 版本没有包含使用 Vulkan 等其他图形 API 的编程任务。

## 任务清单

| 题目 | 一句话主题 |
| --- | --- |
| [HelloWindow](v1-BenchmarkTasks/HelloWindow) | 窗口初始化：GLFW + GLAD + OpenGL 4.6 |
| [CachedCubePipelines](v1-BenchmarkTasks/CachedCubePipelines) | CPU 侧命令/状态缓存绘制 + 双 pipeline 重放 |
| [ProceduralDeferredRenderer](v1-BenchmarkTasks/ProceduralDeferredRenderer) | 多 pass 程序化渲染器：shadow mapping、HDR、后处理 |
| [VoxelPBRFrustumCulling](v1-BenchmarkTasks/VoxelPBRFrustumCulling) | PBR + 实例化 + 可验证的 CPU 视锥剔除 |

## Release v1

Release v1 包含各任务 `Release|x64` 配置下的编译产物（任务结果 zip 包）：

- 发布页：[`releases/tag/v1`](https://github.com/yhcedpn/RenderArena/releases/tag/v1)

> 说明：本版本构建配置仅保留 `Debug|x64` 与 `Release|x64`（见 [#20](https://github.com/yhcedpn/RenderArena/pull/20)：移除 win32 支持）。v1 任务除 VS2026 方法1 外，也可用构建方法 2（CLI + CMake）在 Windows 与 Linux 下构建；其中两份 VoxelPBR 模型产物为 Windows 专有实现（`platforms.json` 仅声明 `windows`，见评审 issue [#17](https://github.com/yhcedpn/RenderArena/issues/17) / [#21](https://github.com/yhcedpn/RenderArena/issues/21)），不参与 Linux 构建。
