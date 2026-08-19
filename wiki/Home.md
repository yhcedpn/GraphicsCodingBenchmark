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