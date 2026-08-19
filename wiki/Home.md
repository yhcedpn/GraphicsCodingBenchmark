# RenderArena Wiki

这里是 `RenderArena` 的文档主页。

## 如何构建

构建配置仅支持 `Debug|x64` 与 `Release|x64`。

### 方法 1：Windows + Visual Studio

在 Windows 11 系统运行，安装最新的 Windows SDK；GPU 需支持 OpenGL 4.6 等最新的图形 API。

1. 安装 Visual Studio 2026，确保安装 `使用 C++ 的桌面开发` 模块。
2. 确保本机已正确集成 `vcpkg` 包管理器，并且 MSVC 版本为 v145。
3. 克隆本仓库，打开 `RenderArena.slnx`，构建并运行你想测试的项目，只可选择 `Debug|x64` 或 `Release|x64` 构建配置。

<!-- 扩容提示：新增构建方式时，在本节下方添加 `### 方法 2：…` 小节，各自列出前提与步骤，注意保持与上面相同的结构。 -->

### 方法 2：CLI + CMake（Windows / Linux）

同一份任务代码可在 Windows 与 Linux 上使用 CMake + vcpkg 构建。

1. 安装 CMake（≥ 3.28）与 Ninja 构建器；Windows 下 CMake 随 VS2026 自带，Linux 下用 `apt install cmake ninja-build`（并安装 GCC）。
2. 设置环境变量 `VCPKG_ROOT` 指向本机 vcpkg 目录。
3. 进入任务目录（如 `OpenGL/HelloWindow_Codex+gpt-5.4@xhigh`）：
   - Windows：`cmake --preset windows-msvc-release`，然后 `cmake --build out/windows-msvc-release --config Release`
   - Linux：`cmake --preset linux-gcc-release`，然后 `cmake --build out/linux-gcc-release`
   - Debug 对应 `windows-msvc-debug` / `linux-gcc-debug`；预设定义集中在仓库根 `CMakePresets.json`，各任务目录的 `CMakePresets.json` 引用同一份。
4. 依赖由各任务目录 `vcpkg.json`（manifest 模式）声明，vcpkg 按 `x64-windows` / `x64-linux` triplet 自动安装；构建产物在任务目录 `out/` 下，不提交。

> 说明：渲染程序需要图形环境运行。Linux 上请在带 X11 会话与 GPU 驱动（或 mesa 软件渲染）的机器上运行；CI 仅做双平台编译验证，不运行。