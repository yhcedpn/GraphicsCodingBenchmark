# ProceduralDeferredRenderer

## 目标

创建一个可运行的 Visual Studio C++ OpenGL 项目，实现一个中小规模但完整的程序化渲染器。该任务综合考察 LearnOpenGL 风格的现代 OpenGL 主题：程序化几何、相机控制、光照、实例化、Framebuffer、阴影贴图、HDR tone mapping、bloom 或 blur 后处理，以及调试视图。

程序必须使用 C++、GLFW 和 GLAD，并以 OpenGL 4.6 core profile 运行为目标。所有场景内容都必须在代码中生成，不得使用外部纹理、模型、字体、HDR 环境贴图、音频或其他资源文件。不得使用任何厂商专属 OpenGL 扩展。

任务重点是考察模型能否组织一个多 pass 渲染流程，正确创建和管理 OpenGL 资源，处理 FBO attachment、深度纹理、shader 数据布局、实例化数据、后处理 ping-pong buffer、运行时调试模式和窗口尺寸变化。

## 要求输出

将完成的项目放在当前目录下。
允许增加本地辅助文件，例如 `*.h`、`*.cpp` 或 `*.glsl`，但所有文件都必须放在同一个结果目录中。程序不得在运行时读取任何外部纹理、模型、字体或其他资产文件。

项目必须满足：

- 使用 OpenGL `4.6` core profile
- 使用 GLFW 创建窗口和处理输入
- 使用 GLAD 加载 OpenGL 函数
- 可在 Visual Studio / MSBuild 的 `Debug|x64` 配置下构建运行

## 功能要求

- 创建一个 `1600 x 900` 窗口，标题为 `OpenGL ProceduralDeferredRenderer Test`。
- 初始化 OpenGL 4.6 core profile，并在控制台输出 GPU vendor、renderer、OpenGL version 和 GLSL version。
- 所有 mesh 必须在代码中程序化生成。至少包含：
  - 一个带索引的 cube mesh，包含 position、normal、UV 或 tangent 中的必要属性。
  - 一个 floor plane。
  - 一个非 cube 程序化 mesh，例如 UV sphere、ico sphere、圆柱、圆环或细分曲面。
- 所有 texture 数据必须在代码中生成，例如 checkerboard、条纹、噪声、渐变、程序化 material lookup texture。不得加载外部图片。
- 场景中至少渲染 `100` 个可见物体。应使用 instancing 或等价的批处理方式渲染大量对象；对象 transform、材质参数和颜色应由固定随机种子或确定性公式生成。
- 实现第一人称相机或 orbit camera，使用透视投影，并基于 delta time 更新移动速度。
- 实现多 pass 渲染流程，至少包含：
  - Shadow pass：从 directional light 或 spot light 视角渲染 depth map。
  - Geometry pass：将场景数据渲染到 framebuffer attachment，或使用结构清晰的 forward/HDR offscreen pass。
  - Lighting pass：应用多个光源和阴影。
  - Post-processing pass：使用 fullscreen triangle 或 fullscreen quad 渲染到默认 framebuffer。
- 必须包含 deferred shading 的 G-buffer，或一个结构清晰的 HDR forward renderer。如果选择 deferred shading，G-buffer 至少应保存 position 或 depth、normal、albedo/material 信息。如果选择 forward/HDR renderer，也必须有离屏 HDR color target 和清晰分离的 lighting/post pass。
- 实现 shadow mapping，并加入 PCF 或其他简单软化策略。阴影必须明显影响 floor 或物体。
- 实现至少 `16` 个程序化 point light；或者实现一个动画 directional/spot light 加至少 `8` 个程序化 point light。光源必须对场景产生可见的明暗变化。
- 在最终 pass 中实现 HDR tone mapping 和 gamma correction。
- 实现一个额外后处理效果，例如 bloom、分离式 Gaussian blur、edge detection、grayscale/debug filter 或 vignette。如果选择 bloom 或 blur，应使用 ping-pong framebuffer。
- 使用 UBO、SSBO、texture buffer 或其他结构化 GPU 数据路径管理 per-frame、per-object 或 per-light 数据。避免在大量循环中用许多零散 uniform 调用传递对象和灯光数据。
- 初始化阶段构建一个小型 CPU 侧 render graph 或 cached pass-command 结构，用来描述 pass 顺序、framebuffer target、shader/pipeline state 和 draw call。每帧应更新动态数据后重放该结构，而不是从零重建全部状态。
- 支持窗口 resize：要么同步重建 framebuffer attachment，要么使用固定内部渲染尺寸并正确设置 viewport 和屏幕映射。
- 提供运行时调试控制：
  - `Esc` 关闭窗口。
  - `W/A/S/D` 加鼠标移动、方向键或其他清晰的代码内控制方案移动相机。
  - `B` 开启/关闭 bloom 或所选额外后处理效果。
  - `H` 开启/关闭 HDR tone mapping。
  - `P` 暂停/恢复动画。
  - `1`、`2`、`3` 在最终渲染结果和至少两个 debug view 之间切换，例如 normal、depth/shadow map、albedo、lighting-only、bloom texture。
- 在窗口标题或其他简单方式中显示 FPS/frame time 和当前 debug mode。无需实现字体渲染。

## 约束

- **绝对禁止**以任何形式读取或写入或查找上级目录内的文件，包括调用工具和执行命令两种形式。
- 所有产物**必须**放在当前目录中，不可以在任何别处。
- 使用当前目录的 `vcpkg.json` 和 `vcpkg_installed/`。
- 不得修改 `vcpkg.json`、triplets、overlays 或共享 installed tree。
- 所有生成的项目文件、源码文件、shader 文件和构建输出都必须保留在当前模型自己的结果目录下。
- 不得下载或加载外部 texture、model、font、cubemap、HDR map、shader include 或其他资产文件。
- 不得使用 `GL_NV_command_list`、bindless graphics、mesh shader、ray tracing API、Vulkan、Direct3D、CUDA、OptiX 或任何厂商专属 OpenGL 扩展。
- 不得使用 `glBegin`/`glEnd` 等废弃 immediate mode。
- 不得依赖 Assimp、stb_image、FreeType、ImGui 或其他资产/UI 库来完成必需功能。
- 如果仓库现有 vcpkg 环境已经提供 GLM，可以使用 GLM；否则应在本地实现所需向量/矩阵数学。
- shader 可以写在 `main.cpp` 字符串中，也可以放在结果目录下的本地 `*.glsl` 文件中，但不得依赖外部 include。

## 验收标准

- 能在 Visual Studio / MSBuild 的 `Debug|x64` 下构建。
- 打开一个可见的 `1600 x 900` 窗口，标题为 `OpenGL ProceduralDeferredRenderer Test`。
- 成功创建 OpenGL 4.6 core profile 上下文，或在失败时输出清晰错误信息。
- 显示一个程序化生成的 3D 场景，包含 floor 和至少 `100` 个可见物体。
- 不依赖任何外部 texture、model、font 或资产文件。
- 至少使用一个 offscreen framebuffer，并通过最终 fullscreen post-processing pass 输出到默认 framebuffer。
- 实现 shadow mapping，阴影能明显影响物体或地面。
- 实现多个动态或程序化光源，且能看到光照差异。
- 实现 HDR tone mapping 和 gamma correction。
- 实现一个额外后处理效果，并可在运行时切换。
- 至少提供两个运行时 debug view。
- 使用结构化 GPU 数据管理共享帧数据、对象数据或灯光数据。
- 初始化时构建 CPU 侧 render graph 或 cached pass-command 结构，并在每帧重放。
- 不在每帧重建静态 OpenGL 资源。
- 相机移动和运行时开关可用，按 `Esc` 能关闭窗口。
- 程序持续渲染直到用户关闭窗口。
