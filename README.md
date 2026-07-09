# OpenGLCodingBenchmark

## 项目简介

`OpenGLCodingBenchmark` 用一组 OpenGL 编码任务，测试不同模型在真实 C++ 图形工程场景中的表现，包括：

- 是否能在约束下生成可构建的 Visual Studio C++ 项目
- 是否能正确使用 `GLFW`、`GLAD`、`OpenGL 4.6 Core Profile` 相关技术栈。
- 是否能组织渲染循环、GPU 资源、输入系统和多 pass 渲染流程

## 如何构建

### 方式一：直接用 Visual Studio

1. 安装 Visual Studio 2026，确保安装 `使用 C++ 的桌面开发` 模块。
2. 确保本机已正确集成 `vcpkg` 包管理器，并且 MSVC 版本为 v145。
3. 构建并运行你想测试的项目，只可选择 `Debug|x64` 或 `Release|x64` 构建配置。

最好是在 Windows 11 系统运行，安装最新的 Windows SDK。另外，确保你使用的 GPU 支持 OpenGL 4.6。这也是作者使用的环境。

## 适合用来观察什么

- 不同模型在简单、中等、复杂 OpenGL 任务上的稳定性差异
- 从“窗口初始化”到“多 pass 渲染”时，代码组织能力如何变化
- 面对资源生命周期、FBO、shader、输入控制时是否容易出错
- 模型是否会编造不存在，不可用的方法，函数，方法参数等
- 在严格约束下，模型是否还能产出结构清晰、可维护的代码
