#pragma once
// Vulkan 1.4 核心引擎：动态渲染 + Synchronization 2 + Push Descriptors。
// 纹理上传：hostImageCopy 支持时用 vkCopyMemoryToImage，否则用额外
// VK_QUEUE_TRANSFER_BIT 队列 + Staging Buffer + vkCmdCopyBufferToImage2 回退。

#include "config.h"
#include "texture_gen.h"
#include "scene.h"

#define VK_NO_PROTOTYPES
#include <volk.h>
#include <vk_mem_alloc.h>

// 补齐本机过渡 headers 缺失的 Host Image Copy 核心 1.4 枚举位
#include "vulkan14_host_image_copy_compat.h"

#include <GLFW/glfw3.h>

// shaderc：用 C 头声明 shaderc_shader_kind，避免在头文件引入 C++ 模板代码
#include <shaderc/shaderc.h>

#include <vector>
#include <string>
#include <cstdint>

namespace rubik {

class VulkanEngine {
public:
    VulkanEngine();
    ~VulkanEngine();
    VulkanEngine(const VulkanEngine&) = delete;
    VulkanEngine& operator=(const VulkanEngine&) = delete;

    // 初始化：创建窗口、实例、设备、交换链、管线、纹理、几何、UBO
    void init(const Config& cfg);

    // 主循环：持续渲染直到窗口关闭
    void run();

    // 释放所有资源（析构会调用，也可显式调用）
    void cleanup();

private:
    // GLFW 回调需要写入私有状态，声明为友元
    friend void framebufferResizeCallback(GLFWwindow*, int, int);
    friend void mouseButtonCallback(GLFWwindow*, int, int, int);
    friend void cursorPosCallback(GLFWwindow*, double, double);
    friend void scrollCallback(GLFWwindow*, double, double);

    // ---- 配置数据（运行时唯一来源） ----
    const Config* cfg_ = nullptr;
    std::vector<PixelTextures> textures_; // 每材质三张
    std::vector<MaterialParams> matParams_;
    std::vector<Vertex> cubeVerts_;
    std::vector<uint32_t> cubeIndices_;
    std::vector<Instance> instances_;
    Camera camera_;

    // ---- GLFW ----
    GLFWwindow* window_ = nullptr;
    int width_ = 1280, height_ = 720;
    bool framebufferResized_ = false;
    // 交互式相机输入状态
    bool mouseDragging_ = false;   // 左键拖拽旋转
    bool panning_ = false;         // 右键拖拽平移
    double lastMouseX_ = 0.0, lastMouseY_ = 0.0;
    double frameTime_ = 0.0;       // 上一帧耗时（秒），用于平滑移动
    void processInput(float dt);
    // 自动验证：环境变量 RUBIK_DUMP 设置时，在第 N 帧转储交换链图像到 PPM
    bool dumpFrame_ = false;
    uint32_t frameCounter_ = 0;
    bool frameDumped_ = false;
    uint32_t dumpImageIndex_ = 0;
    bool dumpPending_ = false;
    void dumpSwapchainImage(uint32_t imageIndex);

    // ---- Vulkan 核心 ----
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    uint32_t graphicsFamily_ = 0;
    uint32_t presentFamily_ = 0;
    uint32_t transferFamily_ = 0;       // 额外 transfer 队列（回退路径）
    bool hasDedicatedTransferQueue_ = false;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue transferQueue_ = VK_NULL_HANDLE;

    // 内存分配器（VMA）
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    // 特性标志
    bool hostImageCopySupported_ = false;
    bool pushDescriptorSupported_ = false;

    // ---- 交换链 ----
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_ = {0, 0};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkImage depthImage_ = VK_NULL_HANDLE;
    VmaAllocation depthImageAlloc_ = VK_NULL_HANDLE;
    VkImageView depthImageView_ = VK_NULL_HANDLE;

    // ---- 命令 ----
    VkCommandPool graphicsCmdPool_ = VK_NULL_HANDLE;
    VkCommandPool transferCmdPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBuffers_;
    // imageAvailable/inFlightFences 按帧 in-flight 索引（currentFrame）
    std::vector<VkSemaphore> imageAvailableSem_;
    std::vector<VkFence> inFlightFences_;
    // renderFinished 按交换链图像索引（imageIndex），避免 MAILBOX 下信号量复用冲突
    std::vector<VkSemaphore> renderFinishedSem_;
    uint32_t currentFrame_ = 0;
    uint32_t maxFramesInFlight_ = 2;

    // ---- 管线 ----
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE; // push descriptor 用

    // ---- UBO（相机） ----
    VkBuffer cameraUbo_ = VK_NULL_HANDLE;
    VmaAllocation cameraUboAlloc_ = VK_NULL_HANDLE;
    void* cameraUboMapped_ = nullptr;

    // ---- 几何缓冲区 ----
    VkBuffer vertexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation vertexAlloc_ = VK_NULL_HANDLE;
    VkBuffer indexBuffer_ = VK_NULL_HANDLE;
    VmaAllocation indexAlloc_ = VK_NULL_HANDLE;
    VkBuffer instanceBuffer_ = VK_NULL_HANDLE;
    VmaAllocation instanceAlloc_ = VK_NULL_HANDLE;

    // ---- 纹理 ----
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation alloc = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    std::vector<Texture> baseColorTex_; // 每材质一张
    std::vector<Texture> roughnessTex_;
    std::vector<Texture> normalTex_;
    VkSampler sampler_ = VK_NULL_HANDLE;

    // ---- 临时上传资源（staging 路径） ----
    std::vector<VkBuffer> stagingBuffers_;
    std::vector<VmaAllocation> stagingAllocs_;
    std::vector<VkFence> uploadFences_; // 等待一次性上传完成

    // ---- 方法 ----
    void createWindow();
    void createInstance();
    void pickPhysicalDevice();
    void createDevice();
    void createSwapchain();
    void createDepthResources();
    void createCommandPools();
    void createSyncObjects();
    void createDescriptorSetLayout();
    void createPipelineAndShaders();
    void createCameraUbo();
    void createGeometryBuffers();
    void createSampler();
    void createTextures(); // 含上传（hostImageCopy 主路径 / transfer queue 回退）

    // 纹理上传两种路径
    void uploadTextureHostImageCopy(const PixelTextures& px, VkFormat baseFmt,
                                    VkFormat roughFmt, VkFormat normFmt,
                                    Texture& outBase, Texture& outRough, Texture& outNorm);
    void uploadTextureStagingQueue(const PixelTextures& px, VkFormat baseFmt,
                                   VkFormat roughFmt, VkFormat normFmt,
                                   Texture& outBase, Texture& outRough, Texture& outNorm);

    void destroySwapchain();
    void recreateSwapchain();
    void drawFrame();
    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex);
    void updateCameraUbo();

    // 工具
    VmaAllocator allocator_internal() const { return allocator_; }
    VkCommandBuffer beginOneTimeGraphics();
    void endOneTimeGraphics(VkCommandBuffer cmd);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
    VkShaderModule compileShader(const std::string& glslSource,
                                 shaderc_shader_kind kind, const char* fileName);
    void transitionImageLayout(VkCommandBuffer cmd, VkImage img, VkFormat fmt,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels, uint32_t arrayLayers,
                               VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage,
                               VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess);

};

} // namespace rubik
