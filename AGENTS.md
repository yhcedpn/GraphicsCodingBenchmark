# RenderArena

## 测评工作规范

下列规范仅适用于完成测评任务时

### 任务目录结构

- 项目文件：`<任务名>.vcxproj` / `.vcxproj.filters`、`CMakeLists.txt`、`CMakePresets.json`、`platforms.json`、`vcpkg.json`
- 代码与资源：`main.cpp`、`shaders/`、`materials.json`、`*_TASK.md`

### 铁律：禁止改动项目文件

以下文件**禁止修改**（内容、参数、属性、依赖一律不动）：

- 任务目录的 `.vcxproj`、`.vcxproj.filters`、`.vcxproj.user`
- 任务目录的 `CMakeLists.txt`、`CMakePresets.json`、`platforms.json`、`vcpkg.json`

### 允许的改动

新增文件是本职工作，允许新建并向 `.vcxproj` / `.vcxproj.filters` **添加对新文件的引用行**（不改其它内容）：

- **源文件**：普通代码（`.cpp`→`ClCompile`、头文件→`ClInclude`）、着色器（`None` 项，filter=源文件）
- **资源文件**：`*_TASK.md`、`materials.json` 等（`None` 项，filter=资源文件）

### 编码约定

- 实现优先写进 `main.cpp`；辅助代码优先用头文件；新编译单元尽量并入 `main.cpp`（避免改 CMake）
- 既有产物中的平台专有代码属被测内容，禁止在项目文件/环境层打补丁，可移植性问题如实记录

### 构建验证（完成任务后应本地构建）

进入任务目录：

```bash
# 配置（Windows / Linux 二选一）
cmake --preset windows-msvc-debug    # 或 windows-msvc-release
cmake --preset linux-gcc-debug       # 或 linux-gcc-release

# 构建
cmake --build out/windows-msvc-debug --config Debug
cmake --build out/linux-gcc-debug
```

- 依赖由 `vcpkg.json` 自动安装（manifest 模式）
- 也可用 VS 打开 `RenderArena.slnx` 构建，配置只能是 `Debug|x64` / `Release|x64`
- `platforms.json` 声明平台：`{"build_platforms": ["windows","linux"]}`，缺失视为全平台；仅 Windows 的任务只列 `windows`，CI 据此跳过 Linux 构建
