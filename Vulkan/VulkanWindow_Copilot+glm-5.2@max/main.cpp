// VulkanWindow —— 用 Vulkan 持续渲染四象限纯色画面的单窗口程序。
//
// 依赖：volk（加载 Vulkan 函数指针）、GLFW（窗口与 Surface）、shaderc（运行时编译 GLSL）、
//   VMA / Vulkan Memory Allocator（内存分配器，本任务未实际分配显存，但已集成并完成初始化）。
// 渲染：一个覆盖全屏的三角形，片元着色器按 uv 坐标在中线处分成四象限纯色。
//   左上=红  左下=白  右下=蓝  右上=绿
// 视口/裁剪使用动态状态，窗口尺寸变化时只需重建交换链相关对象，无需重建管线。

#define VK_NO_PROTOTYPES
#include <volk.h>

// vk_mem_alloc.h 必须在 volk.h 之后包含，VMA 才会启用 volk 专用的函数表导入
// （vmaImportVulkanFunctionsFromVolk）。VMA_IMPLEMENTATION 定义于 vma_impl.cpp。
#include <vk_mem_alloc.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kMaxFramesInFlight = 2;

#define VK_CHECK(expr)                                                      \
    do {                                                                    \
        VkResult _r = (expr);                                               \
        if (_r != VK_SUCCESS) {                                             \
            throw std::runtime_error(std::string(#expr " 失败: ") +         \
                                     std::to_string(static_cast<int>(_r))); \
        }                                                                   \
    } while (0)

// ---- 着色器源码（内嵌，运行时用 shaderc 编译）----
// 顶点：标准全屏三角形技巧，无顶点缓冲区。uv 取覆盖可见区域内的 [0,1]²。
const char* kVertSrc = R"GLSL(
#version 450
layout(location = 0) out vec2 uv;
void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

// 片元：按 uv 的中心线分四象限。Vulkan NDC 中 y 向下，uv.y<0.5 为上半部。
const char* kFragSrc = R"GLSL(
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;
void main() {
    if (uv.x < 0.5 && uv.y < 0.5) {
        fragColor = vec4(1.0, 0.0, 0.0, 1.0); // 左上：红
    } else if (uv.x < 0.5) {
        fragColor = vec4(1.0, 1.0, 1.0, 1.0); // 左下：白
    } else if (uv.y >= 0.5) {
        fragColor = vec4(0.0, 0.0, 1.0, 1.0); // 右下：蓝
    } else {
        fragColor = vec4(0.0, 1.0, 0.0, 1.0); // 右上：绿
    }
}
)GLSL";

VkShaderModule compileShaderModule(VkDevice device, shaderc::Compiler& compiler,
                                   const char* src, shaderc_shader_kind kind) {
    shaderc::CompileOptions options;
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    shaderc::SpvCompilationResult result =
        compiler.CompileGlslToSpv(src, kind, "shader", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::runtime_error(std::string("着色器编译失败: ") + result.GetErrorMessage());
    }
    std::vector<uint32_t> code(result.cbegin(), result.cend());
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = code.size() * sizeof(uint32_t);
    ci.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &module));
    return module;
}

VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
              VkDebugUtilsMessageTypeFlagsEXT /*type*/,
              const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*userData*/) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::cerr << "[validation] " << data->pMessage << "\n";
    }
    return VK_FALSE;
}

void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& ci) {
    ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;
}

VkResult createDebugUtilsMessengerEXT(VkInstance instance,
                                      const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
                                      VkDebugUtilsMessengerEXT* pMessenger) {
    auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (fn) return fn(instance, pCreateInfo, nullptr, pMessenger);
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void destroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT messenger) {
    auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (fn) fn(instance, messenger, nullptr);
}

struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> present;
    bool complete() const { return graphics.has_value() && present.has_value(); }
};

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR caps{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
    bool adequate() const { return !formats.empty() && !presentModes.empty(); }
};

class App {
public:
    void run() {
        initVolk();
        initWindow();
        initVulkan();
        mainLoop();
        cleanup();
    }

private:
    GLFWwindow* window_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    QueueFamilyIndices queueFamily_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{0, 0};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainViews_;
    std::vector<VkFramebuffer> framebuffers_;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkShaderModule vertModule_ = VK_NULL_HANDLE;
    VkShaderModule fragModule_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;

    // 交换链信号量按"每张交换链图像"配一个，用 acquire 返回的 imageIndex 索引：
    // 同一张图像只有在 present 释放其 renderFinished 后才会被再次 acquire，
    // 因此 per-image 信号量不会出现"未释放就复用"的情况。
    // imageAvailable 仍按帧配（acquire 调用时 imageIndex 未知，无法按图像索引），
    // 由 per-frame fence 保证同一帧不会并发 acquire，复用安全。
    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailable_{};
    std::vector<VkSemaphore> renderFinished_;
    // 帧内 fence 按"每帧 in-flight"配一个，用 currentFrame 索引，做 CPU-GPU 帧节流。
    std::array<VkFence, kMaxFramesInFlight> inFlightFences_{};
    uint32_t currentFrame_ = 0;
    bool framebufferResized_ = false;
    bool swapchainValid_ = false;
    bool validationEnabled_ = false;

    // VMA 分配器：已集成并完成初始化（本任务未实际分配显存，保留以备后续扩展）。
    // 必须在 device_ 销毁之前销毁。
    VmaAllocator vmaAllocator_ = VK_NULL_HANDLE;

    // ---- 初始化 ----
    void initVolk() {
        if (volkInitialize() != VK_SUCCESS) {
            throw std::runtime_error("volkInitialize 失败：找不到 Vulkan 加载器");
        }
    }

    void initWindow() {
        if (!glfwInit()) throw std::runtime_error("glfwInit 失败");
        if (!glfwVulkanSupported()) throw std::runtime_error("GLFW：当前环境不支持 Vulkan");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window_ = glfwCreateWindow(800, 600, "VulkanWindow", nullptr, nullptr);
        if (!window_) throw std::runtime_error("glfwCreateWindow 失败");
        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, framebufferSizeCallback);
    }

    static void framebufferSizeCallback(GLFWwindow* w, int /*width*/, int /*height*/) {
        auto* app = static_cast<App*>(glfwGetWindowUserPointer(w));
        app->framebufferResized_ = true;
    }

    void initVulkan() {
        createInstance();
        volkLoadInstance(instance_);
        setupDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        volkLoadDevice(device_);
        createVmaAllocator();
        createSwapchain(VK_NULL_HANDLE);
        createImageViews();
        createRenderPass();
        createShaderModules();
        createPipelineLayout();
        createGraphicsPipeline();
        createFramebuffers();
        createCommandPool();
        createCommandBuffers();
        createSyncObjects();
        createSwapSemaphores();
    }

    void createInstance() {
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
        for (const auto& l : layers) {
            if (std::strcmp(l.layerName, validationLayer) == 0) {
                validationEnabled_ = true;
                break;
            }
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "VulkanWindow";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "NoEngine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_3;

        uint32_t glfwExtCount = 0;
        const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        std::vector<const char*> exts(glfwExts, glfwExts + glfwExtCount);
        if (validationEnabled_) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &appInfo;
        ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
        ci.ppEnabledExtensionNames = exts.data();
        if (validationEnabled_) {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = &validationLayer;
        }

        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
    }

    void setupDebugMessenger() {
        if (!validationEnabled_) return;
        VkDebugUtilsMessengerCreateInfoEXT ci{};
        populateDebugMessengerCreateInfo(ci);
        VK_CHECK(createDebugUtilsMessengerEXT(instance_, &ci, &debugMessenger_));
    }

    void createSurface() {
        VkResult r = glfwCreateWindowSurface(instance_, window_, nullptr, &surface_);
        if (r != VK_SUCCESS) throw std::runtime_error("glfwCreateWindowSurface 失败");
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice d) {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &count, exts.data());
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) return true;
        }
        return false;
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice d) {
        QueueFamilyIndices idx;
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &count, nullptr);
        std::vector<VkQueueFamilyProperties> props(count);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &count, props.data());
        for (uint32_t i = 0; i < count; ++i) {
            if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) idx.graphics = i;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if (present) idx.present = i;
            if (idx.complete()) break;
        }
        return idx;
    }

    SwapchainSupport querySwapchainSupport(VkPhysicalDevice d) {
        SwapchainSupport s;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d, surface_, &s.caps);
        uint32_t fc = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(d, surface_, &fc, nullptr);
        s.formats.resize(fc);
        if (fc) vkGetPhysicalDeviceSurfaceFormatsKHR(d, surface_, &fc, s.formats.data());
        uint32_t pc = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(d, surface_, &pc, nullptr);
        s.presentModes.resize(pc);
        if (pc) vkGetPhysicalDeviceSurfacePresentModesKHR(d, surface_, &pc, s.presentModes.data());
        return s;
    }

    void pickPhysicalDevice() {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (count == 0) throw std::runtime_error("未发现任何 Vulkan GPU");
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());

        int bestScore = -1;
        for (auto d : devices) {
            if (!checkDeviceExtensionSupport(d)) continue;
            SwapchainSupport swap = querySwapchainSupport(d);
            QueueFamilyIndices qf = findQueueFamilies(d);
            if (!qf.complete() || !swap.adequate()) continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(d, &props);
            int score = 250;
            switch (props.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 1000; break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 800; break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 400; break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 100; break;
                default: break;
            }
            if (score > bestScore) {
                bestScore = score;
                physicalDevice_ = d;
                queueFamily_ = qf;
            }
        }
        if (physicalDevice_ == VK_NULL_HANDLE)
            throw std::runtime_error("没有满足图形+呈现+交换链的合适 GPU");
    }

    void createLogicalDevice() {
        std::vector<VkDeviceQueueCreateInfo> queueCis;
        std::set<uint32_t> uniqueFams = {queueFamily_.graphics.value(),
                                          queueFamily_.present.value()};
        float prio = 1.0f;
        for (uint32_t fam : uniqueFams) {
            VkDeviceQueueCreateInfo qci{};
            qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qci.queueFamilyIndex = fam;
            qci.queueCount = 1;
            qci.pQueuePriorities = &prio;
            queueCis.push_back(qci);
        }
        VkPhysicalDeviceFeatures features{};
        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = static_cast<uint32_t>(queueCis.size());
        ci.pQueueCreateInfos = queueCis.data();
        ci.pEnabledFeatures = &features;
        const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        ci.enabledExtensionCount = 1;
        ci.ppEnabledExtensionNames = exts;
        // 校验层在实例层启用即可，设备层不再重复声明。
        VK_CHECK(vkCreateDevice(physicalDevice_, &ci, nullptr, &device_));
        vkGetDeviceQueue(device_, queueFamily_.graphics.value(), 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, queueFamily_.present.value(), 0, &presentQueue_);
    }

    void createVmaAllocator() {
        // VMA 与 volk 协同：VMA_DYNAMIC_VULKAN_FUNCTIONS 默认为 1，
        // 通过 pVulkanFunctions 传入由 volk 加载的函数指针表。
        VmaAllocatorCreateInfo ci{};
        ci.physicalDevice = physicalDevice_;
        ci.device = device_;
        ci.instance = instance_;
        ci.vulkanApiVersion = VK_API_VERSION_1_3; // 与 createInstance 请求的版本一致
        VmaVulkanFunctions funcs{};
        VK_CHECK(vmaImportVulkanFunctionsFromVolk(&ci, &funcs));
        ci.pVulkanFunctions = &funcs;
        VK_CHECK(vmaCreateAllocator(&ci, &vmaAllocator_));
    }

    void destroyAllocator() {
        if (vmaAllocator_) {
            vmaDestroyAllocator(vmaAllocator_);
            vmaAllocator_ = VK_NULL_HANDLE;
        }
    }

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                return f;
        }
        return formats[0];
    }

    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& modes) {
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
        return VK_PRESENT_MODE_FIFO_KHR; // 总是有保障
    }

    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& caps) {
        if (caps.currentExtent.width != UINT32_MAX) return caps.currentExtent;
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        VkExtent2D actual;
        actual.width = std::clamp<uint32_t>(static_cast<uint32_t>(w),
                                            caps.minImageExtent.width, caps.maxImageExtent.width);
        actual.height = std::clamp<uint32_t>(static_cast<uint32_t>(h),
                                             caps.minImageExtent.height, caps.maxImageExtent.height);
        return actual;
    }

    void createSwapchain(VkSwapchainKHR oldSwapchain) {
        SwapchainSupport swap = querySwapchainSupport(physicalDevice_);
        VkSurfaceFormatKHR format = chooseSwapSurfaceFormat(swap.formats);
        VkPresentModeKHR mode = chooseSwapPresentMode(swap.presentModes);
        VkExtent2D extent = chooseSwapExtent(swap.caps);

        if (extent.width == 0 || extent.height == 0) {
            // 窗口被最小化：暂不创建交换链，等待恢复。
            swapchainValid_ = false;
            swapchain_ = VK_NULL_HANDLE;
            swapchainFormat_ = VK_FORMAT_UNDEFINED;
            swapchainExtent_ = {0, 0};
            swapchainImages_.clear();
            return;
        }

        uint32_t imageCount = swap.caps.minImageCount + 1;
        if (swap.caps.maxImageCount > 0 && imageCount > swap.caps.maxImageCount)
            imageCount = swap.caps.maxImageCount;

        VkSwapchainCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        ci.surface = surface_;
        ci.minImageCount = imageCount;
        ci.imageFormat = format.format;
        ci.imageColorSpace = format.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        uint32_t qfams[] = {queueFamily_.graphics.value(), queueFamily_.present.value()};
        if (queueFamily_.graphics != queueFamily_.present) {
            ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            ci.queueFamilyIndexCount = 2;
            ci.pQueueFamilyIndices = qfams;
        } else {
            ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        ci.preTransform = swap.caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = mode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = oldSwapchain;

        VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));
        swapchainFormat_ = format.format;
        swapchainExtent_ = extent;
        swapchainValid_ = true;

        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
        swapchainImages_.resize(imageCount);
        vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());
    }

    void createImageViews() {
        swapchainViews_.resize(swapchainImages_.size());
        for (size_t i = 0; i < swapchainImages_.size(); ++i) {
            VkImageViewCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ci.image = swapchainImages_[i];
            ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ci.format = swapchainFormat_;
            ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
            ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ci.subresourceRange.baseMipLevel = 0;
            ci.subresourceRange.levelCount = 1;
            ci.subresourceRange.baseArrayLayer = 0;
            ci.subresourceRange.layerCount = 1;
            VK_CHECK(vkCreateImageView(device_, &ci, nullptr, &swapchainViews_[i]));
        }
    }

    void createRenderPass() {
        VkAttachmentDescription attachment{};
        attachment.format = swapchainFormat_;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &ref;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask = 0;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        ci.attachmentCount = 1;
        ci.pAttachments = &attachment;
        ci.subpassCount = 1;
        ci.pSubpasses = &subpass;
        ci.dependencyCount = 1;
        ci.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(device_, &ci, nullptr, &renderPass_));
    }

    void createShaderModules() {
        shaderc::Compiler compiler;
        vertModule_ = compileShaderModule(device_, compiler, kVertSrc, shaderc_vertex_shader);
        fragModule_ = compileShaderModule(device_, compiler, kFragSrc, shaderc_fragment_shader);
    }

    void createPipelineLayout() {
        VkPipelineLayoutCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        VK_CHECK(vkCreatePipelineLayout(device_, &ci, nullptr, &pipelineLayout_));
    }

    void createGraphicsPipeline() {
        VkPipelineShaderStageCreateInfo stages[2];
        stages[0] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertModule_;
        stages[0].pName = "main";
        stages[1] = {};
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragModule_;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1; // 视口/裁剪为动态状态

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.depthClampEnable = VK_FALSE;
        raster.rasterizerDiscardEnable = VK_FALSE;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.lineWidth = 1.0f;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.sampleShadingEnable = VK_FALSE;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.logicOpEnable = VK_FALSE;
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;

        VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynState{};
        dynState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynState.dynamicStateCount = 2;
        dynState.pDynamicStates = dynStates;

        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vertexInput;
        ci.pInputAssemblyState = &inputAssembly;
        ci.pViewportState = &viewportState;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &ms;
        ci.pColorBlendState = &blend;
        ci.pDynamicState = &dynState;
        ci.layout = pipelineLayout_;
        ci.renderPass = renderPass_;
        ci.subpass = 0;

        VK_CHECK(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_));
    }

    void createFramebuffers() {
        framebuffers_.resize(swapchainViews_.size());
        for (size_t i = 0; i < swapchainViews_.size(); ++i) {
            VkFramebufferCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            ci.renderPass = renderPass_;
            ci.attachmentCount = 1;
            ci.pAttachments = &swapchainViews_[i];
            ci.width = swapchainExtent_.width;
            ci.height = swapchainExtent_.height;
            ci.layers = 1;
            VK_CHECK(vkCreateFramebuffer(device_, &ci, nullptr, &framebuffers_[i]));
        }
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = queueFamily_.graphics.value();
        VK_CHECK(vkCreateCommandPool(device_, &ci, nullptr, &commandPool_));
    }

    void createCommandBuffers() {
        commandBuffers_.resize(kMaxFramesInFlight);
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = commandPool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());
        VK_CHECK(vkAllocateCommandBuffers(device_, &ai, commandBuffers_.data()));
    }

    void createSyncObjects() {
        // per-frame：imageAvailable（acquire 用）+ inFlightFences（帧节流）。
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &imageAvailable_[i]));
            VK_CHECK(vkCreateFence(device_, &fci, nullptr, &inFlightFences_[i]));
        }
    }

    void createSwapSemaphores() {
        // per-image：renderFinished（present 用），按交换链图像数配一个。
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        renderFinished_.resize(swapchainImages_.size());
        for (size_t i = 0; i < swapchainImages_.size(); ++i) {
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &renderFinished_[i]));
        }
    }

    // ---- 主循环 ----
    void mainLoop() {
        while (!glfwWindowShouldClose(window_)) {
            glfwPollEvents();
            drawFrame();
        }
        vkDeviceWaitIdle(device_);
    }

    void drawFrame() {
        if (!swapchainValid_) {
            int w = 0, h = 0;
            glfwGetFramebufferSize(window_, &w, &h);
            if (w == 0 || h == 0) return; // 仍处于最小化，空转等待
            recreateSwapchain();
            if (!swapchainValid_) return;
        }

        VK_CHECK(vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX));

        uint32_t imageIndex = 0;
        VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                           imageAvailable_[currentFrame_], VK_NULL_HANDLE, &imageIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("vkAcquireNextImageKHR 失败");
        }

        // 即便本次获取是 SUBOPTIMAL，也先画一帧再在 present 后重建。
        framebufferResized_ = framebufferResized_ || (r == VK_SUBOPTIMAL_KHR);

        VK_CHECK(vkResetFences(device_, 1, &inFlightFences_[currentFrame_]));

        recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSem[] = {imageAvailable_[currentFrame_]};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = waitSem;
        submit.pWaitDstStageMask = waitStages;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffers_[currentFrame_];
        VkSemaphore sigSem = renderFinished_[imageIndex];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &sigSem;

        VK_CHECK(vkQueueSubmit(graphicsQueue_, 1, &submit, inFlightFences_[currentFrame_]));

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &sigSem;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &imageIndex;
        VkResult presR = vkQueuePresentKHR(presentQueue_, &present);

        if (presR == VK_ERROR_OUT_OF_DATE_KHR || presR == VK_SUBOPTIMAL_KHR || framebufferResized_) {
            framebufferResized_ = false;
            recreateSwapchain();
        } else if (presR != VK_SUCCESS) {
            throw std::runtime_error("vkQueuePresentKHR 失败");
        }

        currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
    }

    void recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = renderPass_;
        rpBegin.framebuffer = framebuffers_[imageIndex];
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = swapchainExtent_;
        std::array<VkClearValue, 1> clear{};
        clear[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        rpBegin.clearValueCount = static_cast<uint32_t>(clear.size());
        rpBegin.pClearValues = clear.data();

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapchainExtent_.width);
        viewport.height = static_cast<float>(swapchainExtent_.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapchainExtent_;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRenderPass(cmd);
        VK_CHECK(vkEndCommandBuffer(cmd));
    }

    void recreateSwapchain() {
        vkDeviceWaitIdle(device_);
        destroyFramebuffers();
        destroyPipeline();
        destroyRenderPass();
        destroyImageViews();
        destroySwapSemaphores(); // 交换链图像数可能变化，按新数量重建
        VkSwapchainKHR old = swapchain_;
        swapchain_ = VK_NULL_HANDLE;
        createSwapchain(old);
        if (old) vkDestroySwapchainKHR(device_, old, nullptr);
        if (!swapchainValid_) return;
        createImageViews();
        createRenderPass();
        createGraphicsPipeline();
        createFramebuffers();
        createSwapSemaphores();
    }

    // ---- 销毁辅助 ----
    void destroyFramebuffers() {
        for (auto fb : framebuffers_)
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        framebuffers_.clear();
    }
    void destroyPipeline() {
        if (pipeline_) {
            vkDestroyPipeline(device_, pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
        }
    }
    void destroyPipelineLayout() {
        if (pipelineLayout_) {
            vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
            pipelineLayout_ = VK_NULL_HANDLE;
        }
    }
    void destroyShaderModules() {
        if (vertModule_) {
            vkDestroyShaderModule(device_, vertModule_, nullptr);
            vertModule_ = VK_NULL_HANDLE;
        }
        if (fragModule_) {
            vkDestroyShaderModule(device_, fragModule_, nullptr);
            fragModule_ = VK_NULL_HANDLE;
        }
    }
    void destroyRenderPass() {
        if (renderPass_) {
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }
    }
    void destroyImageViews() {
        for (auto v : swapchainViews_)
            if (v) vkDestroyImageView(device_, v, nullptr);
        swapchainViews_.clear();
    }
    void destroyCommandPool() {
        if (commandPool_) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
        }
    }
    void destroySyncObjects() {
        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
            if (imageAvailable_[i]) vkDestroySemaphore(device_, imageAvailable_[i], nullptr);
            if (inFlightFences_[i]) vkDestroyFence(device_, inFlightFences_[i], nullptr);
            imageAvailable_[i] = VK_NULL_HANDLE;
            inFlightFences_[i] = VK_NULL_HANDLE;
        }
    }
    void destroySwapSemaphores() {
        for (auto s : renderFinished_)
            if (s) vkDestroySemaphore(device_, s, nullptr);
        renderFinished_.clear();
    }

    void cleanup() {
        vkDeviceWaitIdle(device_);
        destroyFramebuffers();
        destroyPipeline();
        destroyPipelineLayout();
        destroyShaderModules();
        destroyRenderPass();
        destroyImageViews();
        if (swapchain_) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        destroyCommandPool();
        destroySwapSemaphores();
        destroySyncObjects();
        destroyAllocator(); // VMA allocator 依赖 device，须在 vkDestroyDevice 之前销毁
        if (surface_) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (debugMessenger_) {
            destroyDebugUtilsMessengerEXT(instance_, debugMessenger_);
            debugMessenger_ = VK_NULL_HANDLE;
        }
        if (device_) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (instance_) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        if (window_) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
        glfwTerminate();
        volkFinalize();
    }
};

} // namespace

int main() {
    try {
        App app;
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "致命错误: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
