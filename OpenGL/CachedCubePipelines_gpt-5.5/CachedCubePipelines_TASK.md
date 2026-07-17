# CachedCubePipelines

## 目标

创建一个可运行的 Visual Studio C++ OpenGL 项目，用标准 OpenGL 4.6 实现一个“不依赖厂商扩展的命令/状态缓存绘制”示例：程序在 CPU 侧缓存绘制指令和渲染状态，然后使用两个不同的渲染 pipeline 绘制一个由多个 cube 组成的 3D 场景。

程序必须使用 C++、GLFW 和 GLAD。不得使用 `GL_NV_command_list`、bindless graphics 或任何厂商专属 OpenGL 扩展。任务重点是考察模型能否用标准 OpenGL 资源管理、VAO/VBO/EBO、shader、UBO、深度测试、相机矩阵和状态切换优化，模拟“缓存绘制指令并重放”的开发模式。

最终画面应显示一个可移动相机观察的 cube 场景。一部分 cube 使用带光照的实体渲染 pipeline，另一部分 cube 使用明显不同的 pipeline，例如纯色、线框、法线可视化或描边效果。

## 要求输出

将完成后的项目放在当前目录下。
允许增加本地辅助文件，例如 `*.hpp`、`*.cpp` 或 `*.glsl`，等等，但所有文件都必须放在同一个结果目录中。程序不得依赖任何外部纹理、模型、字体、图片或其他运行时资源。

项目必须满足：

- 使用 OpenGL `4.6` core profile
- 使用 GLFW 创建窗口和处理输入
- 使用 GLAD 加载 OpenGL 函数
- 可在 Visual Studio / MSBuild 的 `Debug|x64` 配置下构建运行

## 功能要求

- 创建一个 `1280 x 720` 窗口，标题为 `OpenGL CachedCubePipelines Test`。
- 初始化 OpenGL 4.6 core profile，并在控制台输出 OpenGL version/vendor/renderer 信息。
- 所有几何数据必须在代码中生成。至少包含一个带索引的 cube mesh，顶点数据应包含 position、normal，并包含 vertex color 或程序化 UV。
- 至少渲染 `48` 个 cube，排布为 3D 网格、环形阵列或其他清晰可见的空间结构。
- 使用透视投影、view 矩阵、depth test 和 face culling。
- 使用标准 OpenGL 实现两个不同的 pipeline。这里的 pipeline 可以表示为“shader program + 固定功能渲染状态”，也可以使用 OpenGL 标准的 separable program pipeline object。两个 pipeline 必须在画面上明显不同：
  - Pipeline A：带法线光照的实体 cube，至少包含 diffuse 和 specular 或 Blinn-Phong 光照。
  - Pipeline B：另一种视觉风格，例如纯色、线框叠加、法线方向可视化或描边渲染。
- 在初始化阶段构建 CPU 侧缓存绘制指令列表。每条缓存指令应保存重放绘制所需的信息，例如 shader/pipeline id、VAO、index count、transform index、material id、draw mode、depth/cull/wireframe 等状态标记。
- 每帧通过重放缓存指令列表绘制场景。不得每帧重新创建 VAO、VBO、EBO、shader program 或完整命令列表。
- 在执行缓存指令时，按 pipeline 或渲染状态进行分组或排序，尽量避免重复调用 `glUseProgram`、重复绑定 VAO 或重复切换相同状态。
- 使用 UBO 或其他结构化 uniform 更新方式保存每帧共享数据，例如 view、projection、camera position、light position、time。
- 添加一个简单、确定性的动画，例如 cube 缓慢旋转、光源绕场景运动或对象上下浮动。
- 实现键盘控制：
  - `Esc` 关闭窗口。
  - `W/A/S/D` 移动相机，或控制相机围绕场景目标点旋转。
  - `Space` 暂停/恢复动画。
  - `1` 和 `2` 分别切换两个 pipeline 分组的显示状态。

## 约束

- **绝对禁止**以任何形式读取或写入或查找上级目录内的文件，包括调用工具和执行命令两种形式。
- 所有产物**必须**放在当前目录中，不可以在任何别处。
- 使用当前目录的 `vcpkg.json` 和 `vcpkg_installed/`。
- 不得修改 `vcpkg.json`、triplets、overlays 或 installed tree。
- 所有生成的项目文件、源码文件、shader 文件和构建输出都必须保留在当前目录下。
- 不得下载资源或依赖任何外部运行时文件。
- 不得使用 `GL_NV_command_list`、bindless graphics、mesh shader、ray tracing API、Vulkan、Direct3D、CUDA、OptiX 或任何厂商专属 OpenGL 扩展。
- 不得使用 `glBegin`/`glEnd` 等废弃 immediate mode。
- 如果仓库现有 vcpkg 环境已经提供 GLM，可以使用 GLM；否则应在本地实现本任务所需的少量向量/矩阵数学。

## 验收标准

- 能在 Visual Studio / MSBuild 的 `Debug|x64` 下构建。
- 打开一个可见的 `1280 x 720` 窗口，标题为 `OpenGL CachedCubePipelines Test`。
- 成功创建 OpenGL 4.6 core profile 上下文，或在失败时输出清晰错误信息。
- 画面显示至少 `48` 个 cube，且不依赖任何外部纹理或模型文件。
- 同一场景中能看到两个视觉上不同的渲染 pipeline。
- 存在初始化时构建、每帧重放的 CPU 侧缓存绘制指令结构。
- 不在每帧重建 shader、VAO、VBO、EBO 或静态 OpenGL 资源。
- 相机和输入控制可用，按 `Esc` 能关闭窗口。
- depth test 和透视投影正确，cube 之间有自然遮挡关系。
- 程序持续渲染直到用户关闭窗口。
