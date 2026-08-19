# RenderArena

## OpenGL 测评项目规范（Visual Studio + msvc + vcpkg）

* 不允许修改 .vcxproj .vcxproj.filters .vcxproj.user 等项目文件（它们已经被事先建立好），除非是为项目增加头文件，源文件或资源文件的引用。
* 主代码文件应当为 main.cpp（它已经被事先建立好）。你也可以建立其他文件（例如辅助代码，着色器，材质或纹理）。

## 跨平台构建规范（Windows + Linux）

* CMake 是跨平台构建契约：每个任务目录提供 `CMakeLists.txt` 与 `CMakePresets.json`（后者 include 仓库根的默认预设），可在 Windows(MSVC) 与 Linux(GCC) 下按 `Debug`/`Release` 构建；vcpkg manifest（`vcpkg.json`）是双平台统一的依赖来源。
* `.vcxproj` 系列文件仅保留 Windows + VS2026 兼容用途，继续遵循「只增不改」；新任务若需要 vcxproj，以 CMake 的 VS 生成器导出，不手写。
* 新增头文件、源文件或着色器等资源时，同步更新对应任务目录的 `CMakeLists.txt`（以及 vcxproj 文件引用）。
* 既有模型评测产物（main.cpp 等）中的平台专有代码属于被测评内容，禁止在环境层打补丁；其可移植性问题如实记录即可。

