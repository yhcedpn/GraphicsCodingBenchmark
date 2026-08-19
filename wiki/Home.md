# GraphicsCodingBenchmark Wiki

这里是 `GraphicsCodingBenchmark` 的文档主索引。

## 如何构建

方法1：在 Windows 系统上使用 Visual Studio

1. 安装 Visual Studio 2026，确保安装 `使用 C++ 的桌面开发` 模块。
2. 确保本机已正确集成 `vcpkg` 包管理器，并且 MSVC 版本为 v145。
3. 克隆本仓库，打开 `GraphicsCodingBenchmark.slnx`，构建并运行你想测试的项目，只可选择 `Debug|x64` 或 `Release|x64` 构建配置。

最好是在 Windows 11 系统运行，安装最新的 Windows SDK。另外，确保你使用的 GPU 支持 OpenGL 4.6 等最新的图形 API。