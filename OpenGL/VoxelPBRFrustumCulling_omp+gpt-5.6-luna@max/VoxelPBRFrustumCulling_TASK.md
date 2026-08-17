# VoxelPBRFrustumCulling

## 目标

创建一个可运行的 Visual Studio C++ OpenGL 项目，渲染一个类似 Minecraft 的像素风格方块场景。场景必须同时展示基于物理的材质、程序化像素纹理、实时光照与阴影、实例化绘制，以及可观察和可验证的 CPU 视锥剔除。

程序必须使用 C++、GLFW、GLAD、GLM 和 nlohmann-json，以 OpenGL 4.6 core profile 为目标。所有几何与纹理都必须在代码中程序化生成，不得依赖外部图片、模型、字体或 HDR 环境贴图。任务目录内预先提供的 `materials.json` 是唯一材质数据源；GLSL shader 必须作为独立文本文件加载，禁止嵌入任何 C++ 源文件或头文件。

任务重点是考察模型能否在保持鲜明像素风格的同时，正确实现 Cook-Torrance PBR、线性颜色工作流、shadow mapping、实例数据管理和保守的 frustum culling，而不是仅用普通漫反射光照模拟金属与塑料。

## 要求输出

将完成后的项目放在当前目录下。
`main.cpp` 必须作为程序主体，包含程序入口以及初始化、主循环和清理流程的主要组织逻辑。允许模型自行增加 `*.h`、`*.hpp`、`*.cpp`、shader 等辅助文件，也允许自行决定这些文件在当前项目内的名称和目录结构；所有产物都必须位于当前结果目录或其子目录中。

所有 shader 源码必须存放在一个或多个独立的 UTF-8 文本文件中，由程序运行时读取。不得把完整或分段的 GLSL 源码、拼接 GLSL 的字符串片段、压缩/编码后的 GLSL，或用于运行时生成 shader 文件的模板集成到 `main.cpp`、其他 `*.cpp` 或任何头文件中。

程序必须使用 nlohmann-json 解析 `materials.json`，并实现 shader 文本读取、编译/链接错误报告和材质字段校验。找不到文件、JSON 语法错误、schema version 不支持、字段缺失或类型错误时，必须列出具体路径/字段并安全退出；不允许静默改用 C++ 内嵌 shader 或硬编码材质作为 fallback。

项目必须满足：

- 使用 OpenGL `4.6` core profile。
- 使用 GLFW 创建窗口和处理输入。
- 使用 GLAD 加载 OpenGL 函数。
- 使用 GLM 完成向量、矩阵、相机和 frustum 数学。
- 使用 nlohmann-json 读取和校验预置的材质 JSON。
- 可在 Visual Studio / MSBuild 的 `Debug|x64` 配置下构建运行。

## 场景布局

采用右手坐标系，`+Y` 为上方。一个方块的边长为 `1.0`，方块位置必须落在整数体素网格上。

- 地面由 `15 x 15` 个独立三维方块组成，共 `225` 个方块。
  - 对每个整数 `x,z ∈ {-7,-6,...,7}` 创建一个中心坐标为 `(x, 0.5, z)`、缩放为 `(1,1,1)` 的 cube 实例，即模型矩阵为 `translate(x,0.5,z)`。
  - 每个地面实例的世界空间 AABB 为 `[(x-0.5,0,z-0.5), (x+0.5,1,z+0.5)]`，地面整体覆盖 `[-7.5,7.5] x [0,1] x [-7.5,7.5]`。
- 在地面的四角和中心建立五根柱子，底部方块放在地面之上，不得替换或穿入地面方块。
  - 柱子底面中心的 `x,z` 对为 `(-7,-7)`、`(-7,7)`、`(7,-7)`、`(7,7)` 和 `(0,0)`。
  - 对每个上述 `(px,pz)` 和每个整数层 `k ∈ {0,1,2,3,4}`，创建完整三维中心坐标为 `(px, 1.5+k, pz)`、缩放为 `(1,1,1)` 的 cube 实例，即模型矩阵为 `translate(px,1.5+k,pz)`。
  - 每根柱子的世界空间范围严格为 `[px-0.5,px+0.5] x [1,6] x [pz-0.5,pz+0.5]`。
- 场景总数必须严格为 `250` 个方块：`225` 个地面方块和 `25` 个柱子方块。
- 使用一个共享的带索引 cube mesh。顶点至少包含 position、normal 和 UV；不得为 250 个方块分别创建 mesh。
- 必须使用 instancing 或等价的批处理方式绘制。静态 transform 和 material id 在初始化时生成，不得每帧重建完整场景。

## 像素风格

- 创建 `1280 x 720` 窗口，标题为 `OpenGL VoxelPBRFrustumCulling Test`。
- 主场景先渲染到固定 `640 x 360` 的离屏颜色与深度目标，再使用 fullscreen triangle 或 quad 以 `GL_NEAREST` 放大到窗口。
- 窗口改变大小时保持 `16:9` 画面比例；允许 letterbox/pillarbox，但放大采样必须保持最近邻，不能用线性过滤模糊像素边缘。
- 所有 base-color、normal 和 ORM 材质纹理必须严格为 `32 x 32`，由 `materials.json` 中的数值和固定 seed 程序化生成，并设置 `GL_NEAREST`。不得接受其他尺寸，也不得将 1×1 常量纹理冒充 32×32 材质。
- 默认关闭 MSAA。允许对阴影做小范围 PCF，但最终颜色不得使用会破坏像素轮廓的 TAA、FXAA 或模糊滤镜。

## PBR 材质与颜色工作流

必须在 fragment shader 中实现 metallic-roughness 工作流的 Cook-Torrance BRDF，至少包含 GGX/Trowbridge-Reitz 法线分布、Smith geometry term 和 Schlick Fresnel。直接光照必须有能量守恒的 diffuse/specular 分配，不能只使用 Blinn-Phong 后把参数命名为 metallic/roughness。

必须加载随任务预置的 `materials.json`，检查根对象的 `schemaVersion` 为 `1`、`textureSize` 为 `32`，并实现以下两种固定材质。表中的数值不得在 C++ 或 GLSL 中复制成另一份“实际值”；JSON 文件是唯一真源：

| section | 用途 | base color（sRGB） | metallic | roughness | AO | normal strength | color variation | roughness variation | seed/pattern |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `brushed_metal` | 225 个地面方块 | `(0.770, 0.780, 0.800)` | `1.000` | `0.280` | `1.000` | `0.120` | `0.025` | `0.060` | `1337 / brushed_x` |
| `red_plastic` | 5 根柱子 | `(0.800, 0.025, 0.018)` | `0.000` | `0.380` | `1.000` | `0.045` | `0.015` | `0.040` | `7331 / molded` |

- 为每种材质生成一层严格 `32 x 32` 的 base-color、tangent-space normal 和 ORM 数据；推荐用三个 `GL_TEXTURE_2D_ARRAY`，两层分别对应两种材质。ORM 通道固定为 `R=AO`、`G=roughness`、`B=metallic`。
- `variation` 表示围绕基准值的最大绝对偏移。生成结果必须满足：金属 roughness `[0.220,0.340]`、塑料 roughness `[0.340,0.420]`；base color 的每个 sRGB 通道也在对应 `base_color_variation` 范围内夹取到 `[0,1]`。
- `brushed_x` 应产生主要沿纹理 X 轴的细拉丝/划痕，`molded` 应产生各向较均匀的弱颗粒。normal 的 XY 扰动绝对值不得超过 `normal_strength`，Z 由单位化得到，避免将表面变成凹凸岩石。
- 程序化噪声必须只使用 `texture_seed` 和 texel 整数坐标，且在不同编译配置中产生相同结果。不得使用当前时间、`rand()` 的进程全局状态或未固定 seed 的随机数。
- 每个 cube 面的纹理必须以像素块清晰重复，不能将一张纹理连续拉伸覆盖整个 `15 x 15` 地面。
- base-color 纹理按 sRGB 数据处理，normal/ORM 按线性数据处理。所有光照计算在线性空间进行，最终输出先做 tone mapping，再做 gamma correction 或正确使用 sRGB framebuffer；不得重复 gamma 校正。

## 程序化 IBL

除直接光外，必须按 split-sum approximation 实现完整的 diffuse/specular IBL，不能再用常量或半球 ambient 代替。不得加载外部 HDR 图片；环境数据在初始化时通过 shader 程序化生成：

- 创建每面 `256 x 256`、`GL_RGB16F` 的 HDR environment cubemap。对单位方向 `d`：
  - 若 `d.y >= 0`，令 `t = clamp(d.y,0,1)`，线性 HDR radiance 为 `mix((0.48,0.576,0.80), (0.12,0.27,0.675), pow(t,0.35))`。
  - 若 `d.y < 0`，线性 HDR radiance 固定为 `(0.035,0.030,0.025)`。
- 从 environment cubemap 卷积生成每面 `32 x 32`、`GL_RGB16F` 的 irradiance cubemap，用于间接漫反射。
- 使用 GGX importance sampling 生成每面基础尺寸 `128 x 128`、`GL_RGB16F` 的 prefiltered environment cubemap，严格包含 `5` 个 mip level；`roughness = mip / 4.0`。
- 生成 `256 x 256`、`GL_RG16F` 的 BRDF integration LUT，R/G 分别存储 split-sum 的 scale/bias。prefilter 与 BRDF 积分每个输出至少使用 `512` 个 Hammersley/GGX 样本。
- 间接光必须按以下数据流组合：`diffuse = irradiance * albedo`；`specular = prefilteredColor * (F * brdf.x + brdf.y)`；最终 ambient 为 `(kD * diffuse + specular) * AO`。
- IBL 的 Fresnel 必须使用 roughness-aware Schlick 形式，采样 prefilter mip 时根据 roughness 选择 LOD。environment/irradiance/prefilter 可使用线性或三线性过滤；`GL_NEAREST` 的硬性要求仅针对 32×32 方块材质与最终像素画面放大。
- 所有 IBL 预计算只在初始化或明确的资源重建阶段执行，不得每帧重新卷积。

## 光照与阴影

- 实现一个暖白色 directional sun：
  - 世界空间光线传播方向为 `normalize(-0.45, -1.0, -0.35)`。
  - 线性空间 radiance/color 为 `(4.5, 4.2, 3.8)`。
- 实现四个较弱的 point light，用于在金属地面和红色塑料上形成可辨认的局部高光：
  - 位置分别为 `(-5, 6.5, -5)`、`(-5, 6.5, 5)`、`(5, 6.5, -5)`、`(5, 6.5, 5)`。
  - 相邻灯使用暖色 `(1.0, 0.55, 0.28)` 与冷色 `(0.28, 0.55, 1.0)` 交替，强度为 `35.0`。
  - 使用物理合理的 inverse-square attenuation；可以设置有限影响半径，但不得使用任意常数/线性/二次衰减三项拼凑公式。
- directional sun 必须生成 shadow map，分辨率至少 `2048 x 2048`。阴影应使用 slope-aware bias，并至少使用 `3 x 3` PCF，避免明显 shadow acne，同时不能让柱子阴影脱离接触面。
- 五根柱子必须在地面上投下清晰可见的阴影。point light 不要求生成阴影。
- 使用 Reinhard 或 ACES approximation 做 tone mapping，并在最终输出正确处理 gamma。

## 相机与交互

- 使用透视投影，初始相机位置建议为 `(18, 14, 18)`，初始观察目标为 `(0, 2.5, 0)`，垂直 FOV 为 `60` 度，near/far plane 为 `0.1/100.0`。
- 相机移动必须基于 delta time：
  - `W/A/S/D` 前后左右移动。
  - 鼠标控制 yaw/pitch；pitch 必须限制以避免翻转。
  - `Esc` 释放鼠标；再次单击窗口可重新捕获鼠标。若鼠标已释放，`Esc` 关闭窗口。
- 运行时控制：
  - `C` 开启/关闭 CPU 视锥剔除。
  - `1` 显示最终 PBR 结果。
  - `2` 显示 albedo。
  - `3` 显示 world-space normal。
  - `4` 显示 roughness/metallic 调试视图。
  - `5` 显示 directional shadow factor。

## 视锥剔除与提交要求

- 每帧根据当前 `projection * view` 矩阵提取并归一化六个世界空间 frustum plane。
- 为每个 cube 实例维护 world-space AABB 或 bounding sphere，并对六个平面做保守的相交测试。与视锥相交的方块必须保留，不能因只测试中心点而在屏幕边缘突然消失。
- 开启剔除时，只把通过测试的实例写入当帧可见 instance buffer，或生成等价的紧凑可见索引列表；实际 instanced draw 的 `instanceCount` 必须等于该材质的可见实例数。仅在 shader 中丢弃不可见实例不算完成剔除。
- 地面金属和红色塑料可以分别提交，每帧主场景的 cube draw call 应不超过 `2` 次；shadow pass 可以使用独立的可见列表并额外提交。
- shadow pass 应从光源视锥执行独立剔除，或保守提交全部 250 个实例。不得错误复用相机可见列表，以免相机视野外但仍能投影到画面内的物体停止投射阴影。
- 按 `C` 关闭剔除时，主场景必须提交全部 `250` 个实例；开启后转动或移出场景时，可见实例数必须随视野变化并可降到 `0`。
- 窗口标题必须每秒更新至少一次，显示 FPS、`Visible: N/250`、`Culled: 250-N`、主场景 cube draw-call 数和当前 debug mode。该统计必须反映实际提交数量。

## OpenGL 资源与状态要求

- 启用 depth test 和 back-face culling，设置与 cube winding 一致的 front face。
- 所有 GLSL 源码必须从模型自行安排位置的独立 UTF-8 文本文件读取；文件名、数量和当前项目内的目录结构不作限定。任何 C++ 源文件或头文件中都不得出现 shader 源码或其编码副本。
- 资源定位必须同时支持 Visual Studio 默认工作目录和直接从 `bin/x64/Debug` 启动。允许从当前工作目录或可执行文件路径向上搜索当前项目目录，但找到 `materials.json` 后不得越过该目录继续访问仓库其他位置。
- 使用 UBO、SSBO 或其他结构化 GPU 数据路径传递 per-frame、per-light、per-material 或 per-instance 数据，避免对 250 个对象逐个设置大量 uniform。
- VAO、VBO、EBO、shader program、纹理、framebuffer 和静态实例数据必须在初始化阶段创建，在退出时释放；不得每帧重新创建静态资源或重新编译 shader。
- 动态可见实例缓冲可使用 orphaning、持久映射或明确同步的更新方式，但不得产生越界写入或依赖未完成的 GPU 读取。
- 支持 framebuffer resize，并为零尺寸窗口避免除零或创建无效 attachment。
- 初始化时在控制台输出 GPU vendor、renderer、OpenGL version 和 GLSL version。shader 编译、program 链接和 framebuffer 完整性失败时必须输出清晰错误并安全退出。

## 约束

- **绝对禁止**以任何形式读取、写入或查找上级目录内的文件，包括调用工具和执行命令两种形式。
- 所有产物**必须**放在当前目录中，不可以在任何别处。
- 使用当前目录的 `vcpkg.json` 和 `vcpkg_installed/`。
- 不得修改 `vcpkg.json`、triplets、overlays、installed tree、`.vcxproj`、`.vcxproj.filters` 或 `.vcxproj.user`，除非是为项目增加头文件，源文件或资源文件的引用。
- 除当前项目内预置的 `materials.json` 和模型创建的独立 shader 文本外，不得下载或加载外部 texture、model、font、cubemap、HDR map、shader include 或其他资产。
- 不得使用 `GL_NV_command_list`、bindless graphics、mesh shader、ray tracing API、Vulkan、Direct3D、CUDA、OptiX 或任何厂商专属 OpenGL 扩展。
- 不得使用 `glBegin`/`glEnd` 等废弃 immediate mode。
- 依赖固定为当前 `vcpkg.json` 中的 `glfw3`、`glad`、`glm` 和 `nlohmann-json`。必须使用 GLM 和 nlohmann-json；不得增加 Assimp、stb_image、FreeType、ImGui 或其他依赖。

## 验收标准

- 能在 Visual Studio / MSBuild 的 `Debug|x64` 配置下构建并运行。
- 打开可见的 `1280 x 720` 窗口，并正确创建 OpenGL 4.6 core profile 上下文，失败时给出清晰错误。
- 场景严格包含 `250` 个共享 cube mesh 的实例，布局、材质和柱高符合本任务定义。
- 最终画面有清晰的低分辨率像素轮廓，最近邻放大正确；两种材质纹理均严格为 32×32，不依赖任何外部资产。
- 金属地面和红色塑料柱使用真实 metallic-roughness Cook-Torrance PBR，并可通过调试视图检查材质数据。
- `materials.json` 被 nlohmann-json 实际解析、校验并作为唯一材质真源；所有 shader 独立存放在模型自行安排的文本文件中，修改文件后重新运行能够影响结果，无 C++ 内嵌副本或 fallback。
- 程序化 HDR environment、irradiance cubemap、5-mip prefilter cubemap 和 BRDF LUT 均创建成功，金属地面能显示随视角和 roughness 变化的环境镜面反射。
- directional sun、四个 point light、柱子阴影、HDR tone mapping 和 gamma correction 均正常工作。
- 所有柱子与地面遮挡关系正确，无明显 z-fighting、shadow acne 或严重 peter-panning。
- CPU 视锥剔除实际减少提交的 instance count；转动相机时统计变化，关闭剔除时恢复 `250/250`。
- 视锥边缘没有因非保守测试造成的方块 popping；相机外物体仍能按正确策略参与 directional shadow pass。
- 地面和柱子使用实例化批处理，主场景每帧 cube draw call 不超过 `2` 次。
- 相机、鼠标捕获、剔除开关和五种 debug view 可用，窗口标题统计与实际状态一致。
- 程序正确处理窗口 resize，不在每帧重建静态 OpenGL 资源，并持续渲染直到用户关闭窗口。
