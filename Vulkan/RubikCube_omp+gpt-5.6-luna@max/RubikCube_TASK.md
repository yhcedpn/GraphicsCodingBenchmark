# Vulkan RubikCube 图形编程能力测评题

## 一、任务目标

在当前项目中使用 **C++20** 实现一个可运行的 Vulkan 3D 场景渲染程序。场景包含部分缺失的三阶魔方结构、金属地板、基于物理的材质（PBR）光照，以及由 CPU 程序化生成的材质纹理。

材质参数和纹理生成参数必须在运行时从当前目录的 `materials.json` 读取。除场景布局所需的材质 ID 外，不得在 C++ 或 shader 中重复硬编码 `materials.json` 中的材质数值、纹理尺寸、随机种子或图案参数。

考察目标：

- Vulkan 1.4 核心 API、动态渲染和现代同步 API 的使用；
- Push Descriptors 扩展的正确启用与资源绑定；
- 主机图像拷贝和 GPU 资源生命周期管理；
- PBR 光照、sRGB 颜色空间和法线贴图处理；
- JSON 配置读取、程序化纹理生成、窗口适配、同步和代码质量。

## 二、语言、依赖与工程约束

### 1. 语言与构建

- 使用 C++20，代码必须以 C++20 语言标准编译；
- 使用当前项目已有的构建入口和配置，不假定特定 IDE、操作系统或编译器实现细节；
- 目标平台以当前目录 `platforms.json` 中的声明为准；
- `materials.json` 是运行时外部资源，必须随程序部署并从文件读取，不得将其内容复制到源码、shader 或预生成二进制资源中。

### 2. vcpkg 依赖

使用当前目录 `vcpkg.json` 中声明的依赖，不得引入未声明的第三方库。当前可用依赖包括：

- `glfw3`：创建窗口、处理事件和创建 Vulkan Surface；
- `vulkan`：Vulkan API headers 与 loader；
- `volk`：Vulkan 函数加载；
- `vulkan-memory-allocator`：可选，用于 Vulkan 内存分配；
- `vulkan-utility-libraries`：可选，用于 Vulkan 辅助功能；
- `shaderc`：可选，用于运行时或构建时 shader 编译；
- `spirv-tools`：可选，用于 SPIR-V 处理或验证；
- `spirv-reflect`：可选，用于 SPIR-V 资源反射；
- `nlohmann-json`：读取和校验 `materials.json`。

不要求使用全部依赖，但 JSON 配置必须使用 `nlohmann-json` 读取，不能退回到硬编码材质参数或预生成材质数据。

## 三、配置驱动的材质规则

### 1. 配置文件读取

程序启动时必须读取当前目录的 `materials.json`，并使用 `nlohmann::json` 解析。文件缺失、无法解析、`schemaVersion` 不支持、`textureSize` 无效、材质数组为空、材质 ID 重复，或材质缺少必需字段时，程序必须报告明确错误并正常退出，不得使用内置默认值继续渲染。

配置文件根对象包含以下字段：

- `schemaVersion`：配置 Schema 版本；
- `textureSize`：所有程序化材质纹理的宽度和高度；
- `materials`：材质对象数组。

每个材质对象必须从 JSON 读取以下字段：

- `id`；
- `baseColorSRGB`；
- `metallic`；
- `roughness`；
- `ambientOcclusion`；
- `normalStrength`；
- `baseColorVariation`；
- `roughnessVariation`；
- `textureSeed`；
- `pattern`。

字段名称、数据类型和数组长度必须校验。材质 ID 必须唯一；场景引用的材质 ID 必须能在 `materials` 数组中解析到对应对象。材质数值的实际值、材质数量和纹理尺寸均以 `materials.json` 当前内容为唯一来源。

### 2. 场景材质引用

- 底层魔方方块引用 `id` 为 `brushed_metal` 的材质对象；
- 中层和顶层魔方方块引用 `id` 为 `red_plastic` 的材质对象；
- 地板引用 `id` 为 `brushed_metal` 的材质对象。

上述 ID 是场景布局的材质引用，不是材质参数定义。所有被引用对象的参数必须来自配置文件；不得在代码中为这些 ID 另行维护数值副本。

### 3. 程序化纹理生成

对 `materials.json` 中的每个材质生成三张纹理：基础色贴图、粗糙度贴图和法线贴图。每张纹理的宽度和高度都必须使用根级 `textureSize`，不得在代码中另设固定尺寸。

生成规则如下：

- 生成在 CPU 端完成；
- 使用对应材质的 `textureSeed` 作为随机数种子，使用确定性的算法，保证相同配置产生相同结果；
- `pattern` 为 `brushed_x` 时，沿 X 轴方向生成高频拉丝条纹，并叠加低频 Perlin 或 Value 噪声，以模拟金属表面；
- `pattern` 为 `molded` 时，生成均匀分布的微小颗粒凹凸，以模拟注塑塑料表面；
- 基础色贴图以 `baseColorSRGB` 为基准，按照 `baseColorVariation` 叠加随机波动；
- 粗糙度贴图以 `roughness` 为基准，按照 `roughnessVariation` 叠加随机波动；
- 法线贴图由高度图推导，整体凹凸强度按照 `normalStrength` 缩放；
- 生成结果必须限制在对应纹理格式的合法范围内。

纹理上传必须使用 Vulkan 1.4 的主机图像拷贝接口 `vkCopyMemoryToImage`，直接从主机内存上传到 Vulkan 图像对象。禁止使用暂存缓冲（Staging Buffer）或 `vkCmdCopyBufferToImage` 完成这些纹理的上传。

## 四、场景详细规格

### 1. 坐标系

- 使用右手坐标系；
- Y 轴向上，X 轴向右，Z 轴指向屏幕内；
- 世界坐标原点位于场景几何中心的 XZ 平面中心；
- 所有方块均为边长 1 个单位的立方体。

### 2. 魔方结构

魔方共三层，自下而上。每层格子索引为 `(xIndex, zIndex)`，取值范围为 0～2：索引 0 对应坐标 -1，索引 1 对应坐标 0，索引 2 对应坐标 +1。方块中心的 Y 坐标分别为 0.5、1.5、2.5。

#### 第 1 层（底层）

- 材质引用：`brushed_metal`；
- 全部 9 个方块均存在；
- 坐标范围：x ∈ [-1, 2]，z ∈ [-1, 2]，y ∈ [0, 1]。

#### 第 2 层（中间层）

- 材质引用：`red_plastic`；
- 存在方块共 6 个：`(0,0)`、`(0,1)`、`(0,2)`、`(1,0)`、`(1,1)`、`(2,0)`；
- 缺失方块共 3 个：`(1,2)`、`(2,1)`、`(2,2)`；
- 坐标范围：y ∈ [1, 2]。

#### 第 3 层（顶层）

- 材质引用：`red_plastic`；
- 存在方块共 3 个：`(0,0)`、`(0,1)`、`(1,2)`；
- 缺失方块共 6 个：`(0,2)`、`(1,0)`、`(1,1)`、`(2,0)`、`(2,1)`、`(2,2)`；
- 坐标范围：y ∈ [2, 3]。

### 3. 地板结构

- 材质引用：`brushed_metal`；
- 由 9×9 个单位立方体平铺构成；
- 地板顶面与魔方底层底面齐平，即 y=0；
- x/z 方向索引范围为 0～8，对应坐标 -4～+4；
- 整体尺寸为 9×9×1 个单位，中心与世界原点对齐；
- 坐标范围：x ∈ [-4.5, 4.5]，z ∈ [-4.5, 4.5]，y ∈ [-1, 0]。

## 五、光照与 PBR

### 1. 光照设置

- 光源类型：单方向平行光；
- 光源位置参考点：地板右下顶点正上方，世界坐标 `(4.5, 6.0, 4.5)`；
- 光线方向：从参考点指向魔方几何中心 `(0, 1.5, 0)`，归一化后约为 `(-0.577, -0.577, -0.577)`；
- 光源颜色：纯白色 `(1.0, 1.0, 1.0)`，强度 1.0；
- 环境光：固定强度 0.03 的白色环境光，并与从 JSON 读取的 `ambientOcclusion` 相乘。

### 2. 光照模型

采用标准 Cook-Torrance PBR 直接光照模型，至少包含：

- 漫反射项：Lambert 模型；
- 高光项：GGX 法线分布、Schlick-GGX 几何函数和 Fresnel-Schlick 近似；
- 金属度工作流：金属区域不产生漫反射，反射颜色由基础色决定；
- `metallic`、`roughness`、`ambientOcclusion` 和纹理生成参数均来自已加载的材质对象。

正确处理 sRGB 颜色空间：基础色数据必须按 sRGB 语义存储和采样，并在 PBR 计算中使用线性空间值；最终输出必须写入 sRGB 交换链图像，由硬件完成最终的线性到 sRGB 转换。不得对同一数据重复进行 gamma 转换。

## 六、Vulkan API 与渲染要求

### 1. API 版本和扩展

- 使用 Vulkan 1.4；
- 动态渲染（Dynamic Rendering）和 Synchronization 2 使用 Vulkan 1.4 核心 API；
- Push Descriptors 使用 `VK_KHR_push_descriptor` 扩展及 `vkCmdPushDescriptorSetKHR`。该扩展不是 Vulkan 1.4 核心的一部分，必须检查物理设备的扩展支持，并在创建设备时正确启用；
- Host Image Copy 使用 Vulkan 1.4 核心的 `vkCopyMemoryToImage`；
- 除 `VK_KHR_push_descriptor` 外，不得依赖厂商专有扩展或未在任务中允许的扩展；
- 所需 API 版本、扩展和 feature 不可用时，必须报告明确错误并退出，不得静默降级到不符合本任务约束的路径。

### 2. 强制 API 使用方式

- 禁止创建传统 `VkRenderPass` 和 `VkFramebuffer`；必须使用 `vkCmdBeginRendering`/`vkCmdEndRendering` 动态指定渲染附件；
- 所有内存屏障和布局转换统一使用 `VkImageMemoryBarrier2`、`VkBufferMemoryBarrier2` 与 `vkCmdPipelineBarrier2`；
- 禁止创建描述符池和普通描述符集对象；资源绑定必须通过 Push Descriptor 在命令缓冲录制时直接推送；
- 程序化纹理必须通过 `vkCopyMemoryToImage` 从主机内存直接上传，禁止 Staging Buffer 中转；
- 资源状态转换、主机写入与 GPU 访问之间必须建立正确的可见性和执行依赖，不能依赖未定义的隐式同步。

## 七、基础渲染与窗口要求

- 使用 GLFW 创建一个可见窗口，并持续渲染直到用户关闭窗口；
- 交换链图像使用 sRGB 格式；
- 开启深度测试，比较方式为 `LESS`；
- 开启背面剔除，约定逆时针三角形为正面；
- 支持窗口大小变化，交换链失效或窗口尺寸变化时自动重建交换链，并更新视口和裁剪矩形；
- 相机使用透视投影，初始位置为 `(5, 4, 6)`，看向世界原点，视野角为 60°；
- 窗口关闭后必须正确释放 Vulkan、GLFW、材质纹理和 JSON 配置相关资源并正常退出。

## 八、禁止事项

-  所有修改和生成物仅限当前目录或当前目录的子目录，禁止读取、查找或写入上级目录。
-  使用当前项目已有的依赖声明和构建配置，禁止修改共享配置或已安装依赖树。
-  不允许以硬编码截图、预生成帧或软件窗口绘制冒充 Vulkan 渲染。
- 禁止把 `materials.json` 中的材质参数、纹理尺寸或随机种子复制为 C++/shader 常量；
- 禁止配置文件缺失或解析失败时使用硬编码材质作为 fallback；
- 禁止使用静态截图、预生成帧、多个子窗口、控件或软件窗口绘制冒充 Vulkan 渲染；
- 禁止使用 Staging Buffer 上传程序化纹理；
- 禁止使用传统 Render Pass/Framebuffer 代替动态渲染；
- 禁止创建描述符池或普通描述符集对象代替 Push Descriptors；
- 禁止修改或绕过验证层错误；
- 禁止引入 `vcpkg.json` 之外的第三方依赖。

## 九、验收标准

1. 项目可使用当前环境提供的构建入口完成配置和构建，并以 C++20 标准编译。
2. 程序启动时成功读取并校验 `materials.json`，所有材质数值和纹理尺寸均来自该文件；修改配置中的合法材质参数后，重新运行能影响对应渲染结果。
3. 程序生成每个配置材质的三张纹理，分辨率严格为 JSON 根级 `textureSize` 指定的尺寸，且相同配置下纹理生成结果可复现。
4. 画面包含规定的部分缺失三阶魔方和 9×9 金属地板，方块位置、层数、缺失分布和材质引用符合场景规格。
5. PBR 直接光照、金属度工作流、环境光、法线贴图和 sRGB 处理符合本任务要求，画面无明显颜色空间错误或深度错误。
6. 动态渲染、Synchronization 2、Push Descriptors、Host Image Copy 的 API 使用方式符合强制约束；不存在传统 Render Pass/Framebuffer、描述符池、普通描述符集或纹理 Staging Buffer 上传路径。
7. 窗口保持响应；调整窗口大小后交换链、视口和裁剪矩形正确更新，不出现未覆盖区域、崩溃或明显错位。
8. 程序关闭窗口后正常退出并释放资源。
