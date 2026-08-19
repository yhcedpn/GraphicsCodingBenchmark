# RenderArena

> 改名说明：本仓库由 `GraphicsCodingBenchmark` 更名为 `RenderArena`。新名突出「渲染」（Render）与「竞技测评」（Arena）的定位，比 "Graphics Coding Benchmark" 更简洁、辨识度更高。更早的前身是 `yhcedpn/OpenGLCodingBenchmark` 项目，当时为方便添加使用 OpenGL 以外的图形 API 的图形编程任务测评而更名。

## 项目简介

`RenderArena` 用一组图形编程任务，测试不同模型在真实 C++/C# 图形工程场景中的表现。

结果按图形 API 或技术类别组织在根目录下。每个任务目录通常包含任务定义、模型生成的工程结果以及对应的构建配置：

- `OpenGL/`：现有的 OpenGL 编程任务（用 OpenGL 4.6 Core Profile）及不同模型的完成结果

## 跨平台构建

构建以 CMake 为跨平台契约，Windows 与 Linux 共用同一套任务工程，每个任务目录独立可构建（自带 `CMakeLists.txt`、`vcpkg.json`、`CMakePresets.json`）。

### Windows

- 推荐：打开 `RenderArena.slnx`，在 VS2026 中选择 `Debug|x64` / `Release|x64` 构建（vcpkg manifest 自动装依赖，无需额外配置）。
- 或 CLI + CMake（在任务目录下执行）：
  ```
  cmake --preset windows-msvc-release
  cmake --build out/windows-msvc-release --config Release
  ```

### Linux

- 前置：CMake ≥ 3.28、Ninja、GCC，并设置 `VCPKG_ROOT` 指向本机 vcpkg 目录（首次构建会经 vcpkg 自动编译依赖，较慢）。
- 在任务目录下执行：
  ```
  cmake --preset linux-gcc-release
  cmake --build out/linux-gcc-release
  ```

### 通用说明

- 预设定义集中在仓库根 `CMakePresets.json`（`windows-msvc-*` / `linux-gcc-*`，Debug 对应 `*-debug`）；依赖由各任务目录 `vcpkg.json` manifest 管理，vcpkg 按 `x64-windows` / `x64-linux` triplet 安装；构建产物在任务目录 `out/` 下，不提交。
- 每个任务目录以 `platforms.json` 声明可构建平台（`{"build_platforms": ["windows","linux"]}`，缺失视为全平台支持）。CI 的 [Build Matrix](.github/workflows/build-matrix.yml) 据此统一跳过不适合当前平台的任务；平台专有实现（如两份 VoxelPBR 的 Windows 专有代码，`platforms.json` 仅声明 `windows`）不会在 Linux 构建——可移植性本身属于被测评内容。
- 后续新增的 Vulkan 等图形 API 任务沿用同一构建方案（新增 `Vulkan/` 目录即可）。

## 文档（Wiki）

项目文档位于 [GitHub Wiki](https://github.com/yhcedpn/RenderArena/wiki)，并由仓库 `wiki/` 目录自动同步维护。

- [Wiki 首页](https://github.com/yhcedpn/RenderArena/wiki)：项目介绍与构建指南
- [v1 —— OpenGL 图形编程任务](https://github.com/yhcedpn/RenderArena/wiki/v1-BenchmarkTasks)：v1 版本的任务清单、Release v1 与相关评审 issue

## 适合用来观察什么

- 不同模型在简单、中等、复杂图形编程任务上的稳定性差异
- 从“窗口初始化”到“多 pass 渲染”等复杂渲染流程时，代码组织能力如何变化
- 面对资源生命周期、FBO、shader、输入控制时是否容易出错
- 模型是否会编造不存在，不可用的方法，函数，方法参数等
- 在严格约束下，模型是否还能产出结构清晰、可维护的代码