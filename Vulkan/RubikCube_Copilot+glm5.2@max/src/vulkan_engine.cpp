#include "vulkan_engine.h"

#include <shaderc/shaderc.hpp>
#include <shaderc/env.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>
#include <array>
#include <stdexcept>
#include <algorithm>
#include <chrono>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace rubik {

// ---- 辅助：读 GLSL 文件 -------------------------------------------------
// 按多个候选路径依次尝试，便于从 build 目录或源码目录运行都能找到着色器。
static std::string readFile(const std::string& path) {
    // 候选路径：传入路径（相对 cwd）→ 相对可执行文件所在目录 → 上级源码目录
    std::vector<std::string> candidates;
    candidates.push_back(path);
    // 从传入相对路径派生：shaders/xxx → ../shaders/xxx（build 目录运行时回退到源码）
    {
        std::string p = path;
        if (p.find("shaders/") == 0 || p.find("shaders\\") == 0)
            candidates.push_back("../" + p);
    }
    for (const auto& c : candidates) {
        std::ifstream in(c, std::ios::binary);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
    }
    throw std::runtime_error("无法打开文件: " + path +
                             "（已尝试 " + std::to_string(candidates.size()) + " 个候选路径）");
}

// ---- 构造/析构 ----------------------------------------------------------
VulkanEngine::VulkanEngine() {}

VulkanEngine::~VulkanEngine() {
    cleanup();
}

void VulkanEngine::init(const Config& cfg) {
    cfg_ = &cfg;

    // 自动验证钩子：RUBIK_DUMP 环境变量设置时，第 30 帧转储交换链图像到 PPM。
    // 必须在 createSwapchain 之前读取，以便交换链图像带上 HOST_TRANSFER usage。
    if (std::getenv("RUBIK_DUMP")) dumpFrame_ = true;

    // 1. 生成所有材质纹理（CPU 端，确定性）
    textures_.reserve(cfg.materials.size());
    matParams_.reserve(cfg.materials.size());
    for (const auto& m : cfg.materials) {
        textures_.push_back(generateTextures(m, cfg.textureSize));
        matParams_.push_back(buildMaterialParams(m));
    }

    // 2. 立方体几何
    makeCubeGeometry(cubeVerts_, cubeIndices_);

    // 3. 场景实例（材质索引来自 Config::indexOf）
    int idxBrushed = cfg.indexOf("brushed_metal");
    int idxRed = cfg.indexOf("red_plastic");
    if (idxBrushed < 0) throw std::runtime_error("场景引用材质 brushed_metal 未在配置中找到");
    if (idxRed < 0) throw std::runtime_error("场景引用材质 red_plastic 未在配置中找到");
    // 底层=brushed_metal, 中层=顶层=red_plastic, 地板=brushed_metal
    instances_ = buildSceneInstances(idxBrushed, idxRed, idxRed, idxBrushed);

    // 4. Vulkan 初始化链
    createWindow();
    createInstance();
    pickPhysicalDevice();
    createDevice();
    createSwapchain();
    createDepthResources();
    createCommandPools();
    createSyncObjects();
    createDescriptorSetLayout();
    createPipelineAndShaders();
    createCameraUbo();
    createGeometryBuffers();
    createSampler();
    createTextures();
}

void VulkanEngine::run() {
    auto lastTime = std::chrono::high_resolution_clock::now();
    while (!glfwWindowShouldClose(window_)) {
        glfwPollEvents();
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        frameTime_ = dt;
        processInput(dt);
        drawFrame();
    }
    vkDeviceWaitIdle(device_);
}

void VulkanEngine::cleanup() {
    if (!device_) return;
    vkDeviceWaitIdle(device_);

    // 纹理
    auto destroyTex = [&](Texture& t) {
        if (t.view) vkDestroyImageView(device_, t.view, nullptr);
        if (t.image) vmaDestroyImage(allocator_internal(), t.image, t.alloc);
        t = {};
    };
    for (auto& t : baseColorTex_) destroyTex(t);
    for (auto& t : roughnessTex_) destroyTex(t);
    for (auto& t : normalTex_) destroyTex(t);
    baseColorTex_.clear(); roughnessTex_.clear(); normalTex_.clear();

    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);

    if (instanceBuffer_) vmaDestroyBuffer(allocator_internal(), instanceBuffer_, instanceAlloc_);
    if (indexBuffer_) vmaDestroyBuffer(allocator_internal(), indexBuffer_, indexAlloc_);
    if (vertexBuffer_) vmaDestroyBuffer(allocator_internal(), vertexBuffer_, vertexAlloc_);
    if (cameraUboMapped_) { vmaUnmapMemory(allocator_internal(), cameraUboAlloc_); cameraUboMapped_ = nullptr; }
    if (cameraUbo_) vmaDestroyBuffer(allocator_internal(), cameraUbo_, cameraUboAlloc_);

    // staging 临时资源（buffer 与 allocation 分开销毁，因二者分开存储）
    for (auto f : uploadFences_) if (f) vkDestroyFence(device_, f, nullptr);
    for (size_t i = 0; i < stagingBuffers_.size(); ++i) {
        if (stagingBuffers_[i]) vkDestroyBuffer(device_, stagingBuffers_[i], nullptr);
        if (i < stagingAllocs_.size() && stagingAllocs_[i])
            vmaFreeMemory(allocator_internal(), stagingAllocs_[i]);
    }
    uploadFences_.clear(); stagingAllocs_.clear(); stagingBuffers_.clear();

    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    if (descriptorSetLayout_) vkDestroyDescriptorSetLayout(device_, descriptorSetLayout_, nullptr);

    destroySwapchain();
    if (depthImageView_) vkDestroyImageView(device_, depthImageView_, nullptr);
    if (depthImage_) vmaDestroyImage(allocator_internal(), depthImage_, depthImageAlloc_);

    for (auto s : imageAvailableSem_) if (s) vkDestroySemaphore(device_, s, nullptr);
    for (auto s : renderFinishedSem_) if (s) vkDestroySemaphore(device_, s, nullptr);
    for (auto f : inFlightFences_) if (f) vkDestroyFence(device_, f, nullptr);

    if (graphicsCmdPool_) vkDestroyCommandPool(device_, graphicsCmdPool_, nullptr);
    if (transferCmdPool_) vkDestroyCommandPool(device_, transferCmdPool_, nullptr);

    if (allocator_internal()) vmaDestroyAllocator(allocator_internal());

    if (debugMessenger_) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(instance_, debugMessenger_, nullptr);
    }
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    if (window_) glfwDestroyWindow(window_);

    instance_ = VK_NULL_HANDLE; device_ = VK_NULL_HANDLE;
    window_ = nullptr;
}

// ---- 窗口 ---------------------------------------------------------------
void framebufferResizeCallback(GLFWwindow* w, int, int) {
    auto* eng = static_cast<VulkanEngine*>(glfwGetWindowUserPointer(w));
    if (eng) eng->framebufferResized_ = true;
}

// 鼠标按键：左键拖拽旋转相机，右键拖拽平移目标
void mouseButtonCallback(GLFWwindow* w, int button, int action, int) {
    auto* eng = static_cast<VulkanEngine*>(glfwGetWindowUserPointer(w));
    if (!eng) return;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        eng->mouseDragging_ = (action == GLFW_PRESS);
        if (eng->mouseDragging_) glfwGetCursorPos(w, &eng->lastMouseX_, &eng->lastMouseY_);
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        eng->panning_ = (action == GLFW_PRESS);
        if (eng->panning_) glfwGetCursorPos(w, &eng->lastMouseX_, &eng->lastMouseY_);
    }
}

// 鼠标移动：拖拽时更新 yaw/pitch（旋转）或 target（平移）
void cursorPosCallback(GLFWwindow* w, double x, double y) {
    auto* eng = static_cast<VulkanEngine*>(glfwGetWindowUserPointer(w));
    if (!eng) return;
    double dx = x - eng->lastMouseX_;
    double dy = y - eng->lastMouseY_;
    eng->lastMouseX_ = x;
    eng->lastMouseY_ = y;
    if (eng->mouseDragging_) {
        // 左键：旋转（dx→yaw，dy→pitch）。注意 Vulkan NDC Y 向下，dy 正为向下，
        // 抬高相机应 pitch 增大，故 pitch += dy * 系数
        eng->camera_.yaw   -= float(dx * 0.01);
        eng->camera_.pitch += float(dy * 0.01);
    } else if (eng->panning_) {
        // 右键：平移 target（沿相机右向和上向，按距离缩放使远近一致）
        float cp = std::cos(eng->camera_.pitch);
        // 相机右向 = (cos(yaw),0,-sin(yaw))；上向近似 (−sin(pitch)cos(yaw), cos(pitch), −sin(pitch)sin(yaw))
        float rx = std::cos(eng->camera_.yaw), rz = -std::sin(eng->camera_.yaw);
        float ux = -std::sin(eng->camera_.pitch) * std::cos(eng->camera_.yaw);
        float uy =  std::cos(eng->camera_.pitch);
        float uz = -std::sin(eng->camera_.pitch) * std::sin(eng->camera_.yaw);
        float s = eng->camera_.distance * 0.0015f;
        eng->camera_.target[0] -= float(rx * dx + ux * dy) * s;
        eng->camera_.target[1] -= float(uy * dy) * s;
        eng->camera_.target[2] -= float(rz * dx + uz * dy) * s;
    }
}

// 滚轮：缩放相机距离
void scrollCallback(GLFWwindow* w, double, double yoff) {
    auto* eng = static_cast<VulkanEngine*>(glfwGetWindowUserPointer(w));
    if (!eng) return;
    eng->camera_.distance *= float(1.0 - yoff * 0.1);
}

void VulkanEngine::createWindow() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(width_, height_, "RubikCube Vulkan 1.4", nullptr, nullptr);
    if (!window_) throw std::runtime_error("创建 GLFW 窗口失败");
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    glfwSetMouseButtonCallback(window_, mouseButtonCallback);
    glfwSetCursorPosCallback(window_, cursorPosCallback);
    glfwSetScrollCallback(window_, scrollCallback);
    glfwGetFramebufferSize(window_, &width_, &height_);
}

// ---- 实例 ---------------------------------------------------------------
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[validation] %s\n", data->pMessage);
    return VK_FALSE;
}

void VulkanEngine::createInstance() {
    if (volkInitialize() != VK_SUCCESS)
        throw std::runtime_error("volkInitialize 失败：找不到 libvulkan");

    // Vulkan 1.4
    uint32_t supportedApi = volkGetInstanceVersion();
    if (supportedApi < VK_API_VERSION_1_4)
        throw std::runtime_error("系统 Vulkan loader 不支持 1.4（实际 " +
            std::to_string(VK_API_VERSION_MAJOR(supportedApi)) + "." +
            std::to_string(VK_API_VERSION_MINOR(supportedApi)) + "）");

    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    std::vector<const char*> instExts(glfwExts, glfwExts + glfwExtCount);
    bool enableDebug = true;
    if (enableDebug) instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.enabledExtensionCount = static_cast<uint32_t>(instExts.size());
    ci.ppEnabledExtensionNames = instExts.data();
    // 1.4 核心
    ci.pApplicationInfo = [&]{
        static VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        ai.pApplicationName = "RubikCube";
        ai.applicationVersion = 1;
        ai.pEngineName = "rubik";
        ai.engineVersion = 1;
        ai.apiVersion = VK_API_VERSION_1_4;
        return &ai;
    }();

    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) throw std::runtime_error("vkCreateInstance 失败");
    volkLoadInstance(instance_);

    if (enableDebug) {
        VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debugCallback;
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT");
        if (fn) fn(instance_, &dci, nullptr, &debugMessenger_);
    }

    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) != VK_SUCCESS)
        throw std::runtime_error("glfwCreateWindowSurface 失败");
}

// ---- 物理设备 -----------------------------------------------------------
void VulkanEngine::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) throw std::runtime_error("无可用 Vulkan 物理设备");
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance_, &count, devs.data());

    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceVulkan14Features v14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &v14;
        vkGetPhysicalDeviceFeatures2(d, &f2);

        // 检查 push descriptor 扩展
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, exts.data());
        bool hasPush = false;
        for (auto& e : exts) {
            if (std::strcmp(e.extensionName, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0)
                hasPush = true;
        }
        if (!hasPush) continue;

        // 检查队列族
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qfps(qfc);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qfc, qfps.data());

        int gfx = -1, present = -1, transfer = -1;
        for (uint32_t i = 0; i < qfc; ++i) {
            VkBool32 presentOk = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &presentOk);
            if ((qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentOk) {
                if (gfx < 0) gfx = static_cast<int>(i);
                if (present < 0) present = static_cast<int>(i);
            }
            if ((qfps[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
                !(qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                transfer = static_cast<int>(i); // 额外（非图形）transfer 队列
            }
        }
        if (gfx < 0 || present < 0) continue;

        physicalDevice_ = d;
        graphicsFamily_ = static_cast<uint32_t>(gfx);
        presentFamily_ = static_cast<uint32_t>(present);
        hostImageCopySupported_ = (v14.hostImageCopy == VK_TRUE);
        pushDescriptorSupported_ = true;
        if (transfer >= 0) {
            transferFamily_ = static_cast<uint32_t>(transfer);
            hasDedicatedTransferQueue_ = true;
        }
        return;
    }
    throw std::runtime_error("未找到支持 push descriptor 的物理设备");
}

// ---- 设备 ---------------------------------------------------------------
void VulkanEngine::createDevice() {
    std::vector<VkDeviceQueueCreateInfo> qcis;
    float prio = 1.0f;
    auto addQ = [&](uint32_t fam) {
        // 同一族只创建一次，queueCount=1（多数设备每族 queueCount 为 1）
        for (auto& q : qcis) if (q.queueFamilyIndex == fam) return;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = fam;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        qcis.push_back(qi);
    };
    addQ(graphicsFamily_);
    if (presentFamily_ != graphicsFamily_) addQ(presentFamily_);
    if (hasDedicatedTransferQueue_ && transferFamily_ != graphicsFamily_ &&
        transferFamily_ != presentFamily_) addQ(transferFamily_);

    std::vector<const char*> devExts = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    };

    // Vulkan 1.4 特性链
    VkPhysicalDeviceVulkan14Features v14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    v14.maintenance5 = VK_TRUE; // vkCmdBindIndexBuffer2 等
    v14.hostImageCopy = hostImageCopySupported_ ? VK_TRUE : VK_FALSE;
    // pushDescriptor 是 1.4 核心特性；启用 VK_KHR_push_descriptor 扩展时必须置 TRUE
    v14.pushDescriptor = VK_TRUE;
    // dynamicRendering / synchronization2 属于 1.3 特性，在 v13 结构里设置

    // Vulkan 1.2 特性：运行时描述符数组 + 采样图像数组非均匀索引（用于 nonuniformEXT）
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;
    v12.descriptorBindingVariableDescriptorCount = VK_FALSE; // push descriptor 禁止
    v12.shaderUniformTexelBufferArrayNonUniformIndexing = VK_FALSE;

    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    v13.maintenance4 = VK_TRUE;
    v13.pNext = &v12;
    v14.pNext = &v13;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = static_cast<uint32_t>(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    dci.pNext = &v14;

    if (vkCreateDevice(physicalDevice_, &dci, nullptr, &device_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDevice 失败");
    volkLoadDevice(device_);

    vkGetDeviceQueue(device_, graphicsFamily_, 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, presentFamily_, 0, &presentQueue_);
    if (hasDedicatedTransferQueue_)
        vkGetDeviceQueue(device_, transferFamily_, 0, &transferQueue_);

    // VMA
    VmaAllocatorCreateInfo vaci{};
    vaci.instance = instance_;
    vaci.physicalDevice = physicalDevice_;
    vaci.device = device_;
    vaci.vulkanApiVersion = VK_API_VERSION_1_4;
    // 动态函数表：通过 volk 导入全部函数指针（volk.h 已在 vk_mem_alloc.h 之前包含，
    // VMA 会声明 vmaImportVulkanFunctionsFromVolk 辅助函数）
    VmaVulkanFunctions vfunc;
    vmaImportVulkanFunctionsFromVolk(&vaci, &vfunc);
    vaci.pVulkanFunctions = &vfunc;
    if (vmaCreateAllocator(&vaci, &allocator_) != VK_SUCCESS)
        throw std::runtime_error("vmaCreateAllocator 失败");
}

// ---- 交换链 -------------------------------------------------------------
static VkSurfaceFormatKHR pickSwapFormat(VkPhysicalDevice pd, VkSurfaceKHR s) {
    uint32_t c = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, s, &c, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(c);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, s, &c, fmts.data());
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB ||
            f.format == VK_FORMAT_R8G8B8A8_SRGB)
            return f;
    return fmts[0];
}

static VkPresentModeKHR pickPresentMode(VkPhysicalDevice pd, VkSurfaceKHR s) {
    uint32_t c = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, s, &c, nullptr);
    std::vector<VkPresentModeKHR> modes(c);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, s, &c, modes.data());
    for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}

void VulkanEngine::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);
    VkSurfaceFormatKHR sf = pickSwapFormat(physicalDevice_, surface_);
    VkPresentModeKHR pm = pickPresentMode(physicalDevice_, surface_);

    VkExtent2D extent;
    if (caps.currentExtent.width == 0xFFFFFFFF) {
        extent.width = std::clamp<uint32_t>(width_, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp<uint32_t>(height_, caps.minImageExtent.height, caps.maxImageExtent.height);
    } else {
        extent = caps.currentExtent;
    }
    width_ = static_cast<int>(extent.width);
    height_ = static_cast<int>(extent.height);

    uint32_t imageCount = std::max(caps.minImageCount, maxFramesInFlight_ + 1);
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface_;
    ci.minImageCount = imageCount;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // 自动验证需要从交换链图像读回：加 TRANSFER_SRC 以便拷到临时可读回图像
    // （交换链不支持 HOST_TRANSFER，故走 vkCmdCopyImage -> 临时图像 -> vkCopyImageToMemory）
    if (dumpFrame_) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = pm;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateSwapchainKHR 失败");
    swapchainFormat_ = sf.format;
    swapchainExtent_ = extent;

    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(device_, swapchain_, &imageCount, swapchainImages_.data());

    swapchainImageViews_.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = swapchainImages_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapchainFormat_;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &vci, nullptr, &swapchainImageViews_[i]) != VK_SUCCESS)
            throw std::runtime_error("vkCreateImageView(swapchain) 失败");
    }
}

// ---- 深度资源 -----------------------------------------------------------
void VulkanEngine::createDepthResources() {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = depthFormat_;
    ici.extent = {swapchainExtent_.width, swapchainExtent_.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(allocator_, &ici, &aci, &depthImage_, &depthImageAlloc_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("vmaCreateImage(depth) 失败");

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = depthImage_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = depthFormat_;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &vci, nullptr, &depthImageView_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateImageView(depth) 失败");
}

// ---- 命令池/同步 --------------------------------------------------------
void VulkanEngine::createCommandPools() {
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = graphicsFamily_;
    if (vkCreateCommandPool(device_, &cpi, nullptr, &graphicsCmdPool_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateCommandPool(graphics) 失败");

    if (hasDedicatedTransferQueue_) {
        cpi.queueFamilyIndex = transferFamily_;
        if (vkCreateCommandPool(device_, &cpi, nullptr, &transferCmdPool_) != VK_SUCCESS)
            throw std::runtime_error("vkCreateCommandPool(transfer) 失败");
    }

    cmdBuffers_.resize(maxFramesInFlight_);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = graphicsCmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(cmdBuffers_.size());
    if (vkAllocateCommandBuffers(device_, &ai, cmdBuffers_.data()) != VK_SUCCESS)
        throw std::runtime_error("vkAllocateCommandBuffers 失败");
}

void VulkanEngine::createSyncObjects() {
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    // imageAvailable 与 inFlightFences 按帧 in-flight（currentFrame）分配
    imageAvailableSem_.resize(maxFramesInFlight_);
    inFlightFences_.resize(maxFramesInFlight_);
    for (uint32_t i = 0; i < maxFramesInFlight_; ++i) {
        if (vkCreateSemaphore(device_, &sci, nullptr, &imageAvailableSem_[i]) != VK_SUCCESS ||
            vkCreateFence(device_, &fci, nullptr, &inFlightFences_[i]) != VK_SUCCESS)
            throw std::runtime_error("创建同步对象失败");
    }
    // renderFinished 按交换链图像数量分配（在 createSwapchain 后调用）
    VkSemaphoreCreateInfo rsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderFinishedSem_.assign(swapchainImages_.size(), VK_NULL_HANDLE);
    for (auto& s : renderFinishedSem_) {
        if (vkCreateSemaphore(device_, &rsci, nullptr, &s) != VK_SUCCESS)
            throw std::runtime_error("创建 renderFinished 信号量失败");
    }
}

// ---- 描述符布局（push descriptor） --------------------------------------
// push descriptor 集布局禁止使用 VARIABLE_DESCRIPTOR_COUNT 等 binding flag，
// 因此纹理数组用固定大小（= 材质数，运行时已知），配合 runtimeDescriptorArray
// + shaderSampledImageArrayNonUniformIndexing 特性实现 nonuniform 索引。
void VulkanEngine::createDescriptorSetLayout() {
    // set0: binding0 camera UBO, binding1 baseColor[], binding2 roughness[], binding3 normal[]
    std::array<VkDescriptorSetLayoutBinding, 4> binds{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = static_cast<uint32_t>(cfg_->materials.size());
    binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[2].binding = 2;
    binds[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[2].descriptorCount = static_cast<uint32_t>(cfg_->materials.size());
    binds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[3].binding = 3;
    binds[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[3].descriptorCount = static_cast<uint32_t>(cfg_->materials.size());
    binds[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    // push descriptor 集布局带 PUSH_DESCRIPTOR 标志；不带任何 binding flags
    ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    ci.bindingCount = static_cast<uint32_t>(binds.size());
    ci.pBindings = binds.data();

    if (vkCreateDescriptorSetLayout(device_, &ci, nullptr, &descriptorSetLayout_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateDescriptorSetLayout 失败");
}

// ---- 着色器编译 ---------------------------------------------------------
VkShaderModule VulkanEngine::compileShader(const std::string& glslSource,
                                           shaderc_shader_kind kind,
                                           const char* fileName) {
    shaderc::Compiler compiler;
    shaderc::CompileOptions opts;
    opts.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_4);
    opts.SetForcedVersionProfile(460, shaderc_profile_core);
    shaderc::SpvCompilationResult result =
        compiler.CompileGlslToSpv(glslSource, kind, fileName, opts);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::runtime_error(std::string("Shader 编译失败 [") + fileName + "]: " +
                                 result.GetErrorMessage());
    }
    std::vector<uint32_t> spv(result.cbegin(), result.cend());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * sizeof(uint32_t);
    ci.pCode = spv.data();
    VkShaderModule mod;
    if (vkCreateShaderModule(device_, &ci, nullptr, &mod) != VK_SUCCESS)
        throw std::runtime_error("vkCreateShaderModule 失败");
    return mod;
}

// ---- 管线 ---------------------------------------------------------------
void VulkanEngine::createPipelineAndShaders() {
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout setLayouts[] = {descriptorSetLayout_};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = setLayouts;

    // push constant: mat4(64) + uint(4) + pad(12) + MaterialParams(32) = 112
    VkPushConstantRange pcr;
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = 112;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_) != VK_SUCCESS)
        throw std::runtime_error("vkCreatePipelineLayout 失败");

    std::string vertSrc = readFile("shaders/scene.vert");
    std::string fragSrc = readFile("shaders/scene.frag");
    VkShaderModule vertMod = compileShader(vertSrc, shaderc_vertex_shader, "scene.vert");
    VkShaderModule fragMod = compileShader(fragSrc, shaderc_fragment_shader, "scene.frag");

    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertMod, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragMod, "main", nullptr},
    };

    VkVertexInputBindingDescription vibd{};
    vibd.binding = 0;
    vibd.stride = sizeof(Vertex);
    vibd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription viads[4];
    viads[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)};
    viads[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)};
    viads[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, tangent)};
    viads[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)};

    VkPipelineVertexInputStateCreateInfo visci{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    visci.vertexBindingDescriptionCount = 1;
    visci.pVertexBindingDescriptions = &vibd;
    visci.vertexAttributeDescriptionCount = 4;
    visci.pVertexAttributeDescriptions = viads;

    VkPipelineInputAssemblyStateCreateInfo iasci{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iasci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{0, 0, (float)swapchainExtent_.width, (float)swapchainExtent_.height, 0, 1};
    VkRect2D sc{{0, 0}, swapchainExtent_};
    VkPipelineViewportStateCreateInfo vpsci{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vpsci.viewportCount = 1; vpsci.pViewports = &vp;
    vpsci.scissorCount = 1; vpsci.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rsci{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rsci.cullMode = VK_CULL_MODE_BACK_BIT;
    rsci.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsci.polygonMode = VK_POLYGON_MODE_FILL;
    rsci.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msci{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    msci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dsci{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dsci.depthTestEnable = VK_TRUE;
    dsci.depthWriteEnable = VK_TRUE;
    dsci.depthCompareOp = VK_COMPARE_OP_LESS;
    dsci.depthBoundsTestEnable = VK_FALSE;
    dsci.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cbas{};
    cbas.blendEnable = VK_FALSE;
    cbas.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cbsci{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbsci.attachmentCount = 1;
    cbsci.pAttachments = &cbas;

    // 动态渲染附件格式
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &swapchainFormat_;
    rci.depthAttachmentFormat = depthFormat_;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyci{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyci.dynamicStateCount = 2;
    dyci.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &visci;
    gpci.pInputAssemblyState = &iasci;
    gpci.pViewportState = &vpsci;
    gpci.pRasterizationState = &rsci;
    gpci.pMultisampleState = &msci;
    gpci.pDepthStencilState = &dsci;
    gpci.pColorBlendState = &cbsci;
    gpci.pDynamicState = &dyci;
    gpci.layout = pipelineLayout_;
    gpci.pNext = &rci;

    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateGraphicsPipelines 失败");

    vkDestroyShaderModule(device_, vertMod, nullptr);
    vkDestroyShaderModule(device_, fragMod, nullptr);
}

// ---- 相机 UBO -----------------------------------------------------------
void VulkanEngine::createCameraUbo() {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sizeof(CameraUbo);
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    if (vmaCreateBuffer(allocator_, &bci, &aci, &cameraUbo_, &cameraUboAlloc_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("vmaCreateBuffer(cameraUbo) 失败");
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(allocator_, cameraUboAlloc_, &info);
    cameraUboMapped_ = info.pMappedData;
}

// ---- 几何缓冲区 ---------------------------------------------------------
void VulkanEngine::createGeometryBuffers() {
    auto uploadBuffer = [&](VkBuffer dst, const void* data, VkDeviceSize size) {
        VkBufferCreateInfo sbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        sbci.size = size;
        sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo saci{};
        saci.usage = VMA_MEMORY_USAGE_AUTO;
        saci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        VkBuffer stg; VmaAllocation stgA; VmaAllocationInfo si{};
        if (vmaCreateBuffer(allocator_, &sbci, &saci, &stg, &stgA, &si) != VK_SUCCESS)
            throw std::runtime_error("vmaCreateBuffer(staging) 失败");
        std::memcpy(si.pMappedData, data, size);
        vmaFlushAllocation(allocator_, stgA, 0, size);

        VkCommandBuffer cmd = beginOneTimeGraphics();
        VkBufferCopy region{0, 0, size};
        vkCmdCopyBuffer(cmd, stg, dst, 1, &region);
        endOneTimeGraphics(cmd);
        vmaDestroyBuffer(allocator_, stg, stgA);
    };

    VmaAllocationCreateInfo devAci{}; devAci.usage = VMA_MEMORY_USAGE_AUTO;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};

    // 顶点
    bci.size = sizeof(Vertex) * cubeVerts_.size();
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vmaCreateBuffer(allocator_, &bci, &devAci, &vertexBuffer_, &vertexAlloc_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("vmaCreateBuffer(vertex) 失败");
    uploadBuffer(vertexBuffer_, cubeVerts_.data(), bci.size);

    // 索引
    bci.size = sizeof(uint32_t) * cubeIndices_.size();
    bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vmaCreateBuffer(allocator_, &bci, &devAci, &indexBuffer_, &indexAlloc_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("vmaCreateBuffer(index) 失败");
    uploadBuffer(indexBuffer_, cubeIndices_.data(), bci.size);

    // 实例
    bci.size = sizeof(Instance) * instances_.size();
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vmaCreateBuffer(allocator_, &bci, &devAci, &instanceBuffer_, &instanceAlloc_, nullptr) != VK_SUCCESS)
        throw std::runtime_error("vmaCreateBuffer(instance) 失败");
    uploadBuffer(instanceBuffer_, instances_.data(), bci.size);
}

// ---- 采样器 -------------------------------------------------------------
void VulkanEngine::createSampler() {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxLod = 1.0f;
    if (vkCreateSampler(device_, &sci, nullptr, &sampler_) != VK_SUCCESS)
        throw std::runtime_error("vkCreateSampler 失败");
}

// ---- 纹理创建与上传 -----------------------------------------------------
// hostImageCopy 支持时走 vkCopyMemoryToImage 主路径；否则走额外 transfer queue 回退。
void VulkanEngine::createTextures() {
    const VkFormat baseFmt = VK_FORMAT_R8G8B8A8_SRGB;   // 基础色 sRGB
    const VkFormat roughFmt = VK_FORMAT_R8_UNORM;        // 粗糙度单通道
    const VkFormat normFmt = VK_FORMAT_R8G8B8A8_UNORM;   // 法线

    baseColorTex_.resize(cfg_->materials.size());
    roughnessTex_.resize(cfg_->materials.size());
    normalTex_.resize(cfg_->materials.size());

    for (size_t i = 0; i < cfg_->materials.size(); ++i) {
        if (hostImageCopySupported_) {
            uploadTextureHostImageCopy(textures_[i], baseFmt, roughFmt, normFmt,
                                       baseColorTex_[i], roughnessTex_[i], normalTex_[i]);
        } else {
            if (!hasDedicatedTransferQueue_)
                throw std::runtime_error("hostImageCopy 不支持且无额外 transfer 队列，无法上传纹理");
            uploadTextureStagingQueue(textures_[i], baseFmt, roughFmt, normFmt,
                                      baseColorTex_[i], roughnessTex_[i], normalTex_[i]);
        }
    }
}

// 创建一张 2D 图像 + 视图（OPTIMAL 布局，用于采样）
static void createImageWithView(VkDevice dev, VmaAllocator alloc, VkFormat format,
                                uint32_t size, VkImageUsageFlags usage, VkImage& img,
                                VmaAllocation& imgAlloc, VkImageView& view) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {size, size, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(alloc, &ici, &aci, &img, &imgAlloc, nullptr) != VK_SUCCESS)
        throw std::runtime_error("vmaCreateImage 失败");

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &vci, nullptr, &view) != VK_SUCCESS)
        throw std::runtime_error("vkCreateImageView 失败");
}

// 用 Synchronization2 做布局转换
void VulkanEngine::transitionImageLayout(VkCommandBuffer cmd, VkImage img, VkFormat,
                                         VkImageLayout oldLayout, VkImageLayout newLayout,
                                         uint32_t mipLevels, uint32_t arrayLayers,
                                         VkPipelineStageFlags2 srcStage,
                                         VkPipelineStageFlags2 dstStage,
                                         VkAccessFlags2 srcAccess,
                                         VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = mipLevels;
    b.subresourceRange.layerCount = arrayLayers;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// ---- 主路径：hostImageCopy（vkCopyMemoryToImage） ------------------------
// 直接从主机内存上传到图像，无需 staging buffer。
// 核心 1.4 API：vkCopyMemoryToImage(VkDevice, const VkCopyMemoryToImageInfo*)。
//
// 经 vk.xml 1.4 核心规范确认：核心 1.4 不提供专门的 usage 位、布局或访问位
// （见 vulkan14_host_image_copy_compat.h 说明）。只需启用 hostImageCopy 设备特性，
// 图像 usage 用常规 SAMPLED|TRANSFER_DST，目标布局用 VK_IMAGE_LAYOUT_GENERAL，
// 主机写访问用 VK_ACCESS_2_HOST_WRITE_BIT，拷贝完成后转 SHADER_READ_ONLY_OPTIMAL。
void VulkanEngine::uploadTextureHostImageCopy(const PixelTextures& px, VkFormat baseFmt,
                                              VkFormat roughFmt, VkFormat normFmt,
                                              Texture& outBase, Texture& outRough,
                                              Texture& outNorm) {
    const uint32_t sz = px.size;
    // 基础色 4bpp，粗糙度 1bpp，法线 4bpp
    // host image copy 要求图像带 VK_IMAGE_USAGE_HOST_TRANSFER_BIT（VUID 09113），
    // 另需 SAMPLED 用于采样、TRANSFER_DST 以备 layout 转换路径
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_HOST_TRANSFER_BIT;
    createImageWithView(device_, allocator_, baseFmt, sz, usage,
                        outBase.image, outBase.alloc, outBase.view);
    outBase.format = baseFmt;
    createImageWithView(device_, allocator_, roughFmt, sz, usage,
                        outRough.image, outRough.alloc, outRough.view);
    outRough.format = roughFmt;
    createImageWithView(device_, allocator_, normFmt, sz, usage,
                        outNorm.image, outNorm.alloc, outNorm.view);
    outNorm.format = normFmt;

    // 先用一个图形命令把三张图像转到 GENERAL（接收主机拷贝的目标布局）
    VkCommandBuffer cmd = beginOneTimeGraphics();
    auto toGeneral = [&](VkImage img) {
        transitionImageLayout(cmd, img, VK_FORMAT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_GENERAL, 1, 1,
                              VK_PIPELINE_STAGE_2_NONE, VK_PIPELINE_STAGE_2_HOST_BIT,
                              VK_ACCESS_2_NONE, VK_ACCESS_2_HOST_WRITE_BIT);
    };
    toGeneral(outBase.image);
    toGeneral(outRough.image);
    toGeneral(outNorm.image);
    endOneTimeGraphics(cmd);

    // 主机内存 -> 图像（vkCopyMemoryToImage），目标布局 GENERAL
    auto doCopy = [&](VkImage img, const void* hostData) {
        VkMemoryToImageCopy cpy{VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY};
        cpy.pHostPointer = hostData;
        cpy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cpy.imageSubresource.mipLevel = 0;
        cpy.imageSubresource.baseArrayLayer = 0;
        cpy.imageSubresource.layerCount = 1;
        cpy.imageOffset = {0, 0, 0};
        cpy.imageExtent = {sz, sz, 1};
        // rowLength=0 表示紧密打包
        cpy.memoryRowLength = 0;
        cpy.memoryImageHeight = 0;
        VkCopyMemoryToImageInfo info{VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO};
        info.dstImage = img;
        info.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        info.regionCount = 1;
        info.pRegions = &cpy;
        if (vkCopyMemoryToImage(device_, &info) != VK_SUCCESS)
            throw std::runtime_error("vkCopyMemoryToImage 失败");
    };
    doCopy(outBase.image, px.baseColor.data());
    doCopy(outRough.image, px.roughness.data());
    doCopy(outNorm.image, px.normal.data());

    // 转到 SHADER_READ_ONLY_OPTIMAL 供采样
    cmd = beginOneTimeGraphics();
    auto toSampled = [&](VkImage img) {
        transitionImageLayout(cmd, img, VK_FORMAT_UNDEFINED,
                              VK_IMAGE_LAYOUT_GENERAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1,
                              VK_PIPELINE_STAGE_2_HOST_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_HOST_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    };
    toSampled(outBase.image);
    toSampled(outRough.image);
    toSampled(outNorm.image);
    endOneTimeGraphics(cmd);
}

// ---- 回退路径：额外 transfer queue + staging buffer --------------------
void VulkanEngine::uploadTextureStagingQueue(const PixelTextures& px, VkFormat baseFmt,
                                             VkFormat roughFmt, VkFormat normFmt,
                                             Texture& outBase, Texture& outRough,
                                             Texture& outNorm) {
    const uint32_t sz = px.size;
    const VkImageUsageFlags usageSampled = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                           VK_IMAGE_USAGE_SAMPLED_BIT;
    createImageWithView(device_, allocator_, baseFmt, sz, usageSampled,
                        outBase.image, outBase.alloc, outBase.view);
    outBase.format = baseFmt;
    createImageWithView(device_, allocator_, roughFmt, sz, usageSampled,
                        outRough.image, outRough.alloc, outRough.view);
    outRough.format = roughFmt;
    createImageWithView(device_, allocator_, normFmt, sz, usageSampled,
                        outNorm.image, outNorm.alloc, outNorm.view);
    outNorm.format = normFmt;

    // 一个 transfer 命令缓冲区上传三张
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = transferCmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    auto toTransferDst = [&](VkImage img) {
        transitionImageLayout(cmd, img, VK_FORMAT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 1,
                              VK_PIPELINE_STAGE_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                              VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    };
    toTransferDst(outBase.image);
    toTransferDst(outRough.image);
    toTransferDst(outNorm.image);

    auto doCopy = [&](VkImage img, const void* hostData, size_t dataSize) {
        VkBufferCreateInfo sbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        sbci.size = dataSize;
        sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo saci{};
        saci.usage = VMA_MEMORY_USAGE_AUTO;
        saci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        VkBuffer stg; VmaAllocation stgA; VmaAllocationInfo si{};
        vmaCreateBuffer(allocator_, &sbci, &saci, &stg, &stgA, &si);
        std::memcpy(si.pMappedData, hostData, dataSize);
        vmaFlushAllocation(allocator_, stgA, 0, dataSize);
        stagingBuffers_.push_back(stg); stagingAllocs_.push_back(stgA);

        VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {sz, sz, 1};
        VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
        ci.srcBuffer = stg;
        ci.dstImage = img;
        ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ci.regionCount = 1;
        ci.pRegions = &region;
        vkCmdCopyBufferToImage2(cmd, &ci);
    };
    doCopy(outBase.image, px.baseColor.data(), px.baseColor.size());
    doCopy(outRough.image, px.roughness.data(), px.roughness.size());
    doCopy(outNorm.image, px.normal.data(), px.normal.size());

    auto toSampled = [&](VkImage img) {
        transitionImageLayout(cmd, img, VK_FORMAT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1,
                              VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                              VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    };
    toSampled(outBase.image);
    toSampled(outRough.image);
    toSampled(outNorm.image);
    vkEndCommandBuffer(cmd);

    VkFence fence;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device_, &fci, nullptr, &fence);
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    VkCommandBufferSubmitInfo csi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    csi.commandBuffer = cmd;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &csi;
    if (vkQueueSubmit2(transferQueue_, 1, &si, fence) != VK_SUCCESS)
        throw std::runtime_error("vkQueueSubmit2(transfer) 失败");
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, transferCmdPool_, 1, &cmd);
}

// ---- 工具：一次性图形命令缓冲区 ----------------------------------------
VkCommandBuffer VulkanEngine::beginOneTimeGraphics() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = graphicsCmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VulkanEngine::endOneTimeGraphics(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    VkCommandBufferSubmitInfo csi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    csi.commandBuffer = cmd;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &csi;
    VkFence fence;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device_, &fci, nullptr, &fence);
    if (vkQueueSubmit2(graphicsQueue_, 1, &si, fence) != VK_SUCCESS)
        throw std::runtime_error("vkQueueSubmit2(graphics one-time) 失败");
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(device_, fence, nullptr);
    vkFreeCommandBuffers(device_, graphicsCmdPool_, 1, &cmd);
}

uint32_t VulkanEngine::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1 << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    throw std::runtime_error("找不到匹配的内存类型");
}

// ---- 交换链重建 ---------------------------------------------------------
void VulkanEngine::destroySwapchain() {
    // renderFinished 按交换链图像分配，随交换链销毁重建
    for (auto s : renderFinishedSem_) if (s) vkDestroySemaphore(device_, s, nullptr);
    renderFinishedSem_.clear();
    for (auto v : swapchainImageViews_) if (v) vkDestroyImageView(device_, v, nullptr);
    swapchainImageViews_.clear();
    swapchainImages_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

void VulkanEngine::recreateSwapchain() {
    int w = 0, h = 0;
    while (w == 0 || h == 0) {
        glfwGetFramebufferSize(window_, &w, &h);
        glfwWaitEvents();
    }
    vkDeviceWaitIdle(device_);
    destroySwapchain();
    if (depthImageView_) { vkDestroyImageView(device_, depthImageView_, nullptr); depthImageView_ = VK_NULL_HANDLE; }
    if (depthImage_) { vmaDestroyImage(allocator_, depthImage_, depthImageAlloc_); depthImage_ = VK_NULL_HANDLE; depthImageAlloc_ = VK_NULL_HANDLE; }
    createSwapchain();
    createDepthResources();
    // 重建 renderFinished（数量随交换链图像数变化）
    VkSemaphoreCreateInfo rsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderFinishedSem_.assign(swapchainImages_.size(), VK_NULL_HANDLE);
    for (auto& s : renderFinishedSem_) {
        if (vkCreateSemaphore(device_, &rsci, nullptr, &s) != VK_SUCCESS)
            throw std::runtime_error("重建 renderFinished 信号量失败");
    }
}

// ---- 交互式相机输入处理 -------------------------------------------------
// WASD/方向键平移 target（沿相机右向/前向），QE 上下，R 重置，滚轮已缩放距离。
void VulkanEngine::processInput(float dt) {
    // ESC 退出
    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    // R 重置相机到初始视角
    if (glfwGetKey(window_, GLFW_KEY_R) == GLFW_PRESS) {
        camera_.target[0] = 0.0f; camera_.target[1] = 0.0f; camera_.target[2] = 0.0f;
        camera_.pos[0] = 5.0f; camera_.pos[1] = 4.0f; camera_.pos[2] = 6.0f;
        camera_.orbitInit = false; // 触发重新反推球面坐标
        updateCameraOrbit(camera_);
    }

    // 先确保球面坐标已初始化
    updateCameraOrbit(camera_);

    // 计算相机前向（水平面）与右向，用于平移 target
    float cp = std::cos(camera_.pitch);
    float fx = cp * std::cos(camera_.yaw);
    float fy = std::sin(camera_.pitch);
    float fz = cp * std::sin(camera_.yaw);
    // 相机看向 target，前向 = (target - pos) 归一化 = -(fx,fy,fz)
    // 水平前向（忽略 y）用于 WASD
    float hx = -fx, hz = -fz;
    float hl = std::sqrt(hx*hx + hz*hz); if (hl > 1e-6f) { hx /= hl; hz /= hl; }
    // 右向 = 前向 × up：(hx,0,hz)×(0,1,0) = (-hz,0,hx)
    float rx = -hz, rz = hx;
    float speed = camera_.distance * 1.5f * dt; // 距离越远移速越快
    float dx = 0, dy = 0, dz = 0;
    if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_UP) == GLFW_PRESS)   { dx += hx; dz += hz; }
    if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_DOWN) == GLFW_PRESS) { dx -= hx; dz -= hz; }
    if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_RIGHT) == GLFW_PRESS){ dx += rx; dz += rz; }
    if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_LEFT) == GLFW_PRESS) { dx -= rx; dz -= rz; }
    if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS) dy += 1.0f;
    if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS) dy -= 1.0f;
    camera_.target[0] += dx * speed;
    camera_.target[1] += dy * speed;
    camera_.target[2] += dz * speed;
    (void)fy;
}

// ---- 相机 UBO 更新 ------------------------------------------------------
void VulkanEngine::updateCameraUbo() {
    // 先按球面坐标同步 pos（输入可能改了 yaw/pitch/distance/target）
    updateCameraOrbit(camera_);
    camera_.aspect = static_cast<float>(swapchainExtent_.width) /
                     static_cast<float>(swapchainExtent_.height);
    CameraUbo ubo = buildCameraUbo(camera_, swapchainExtent_.width, swapchainExtent_.height);
    std::memcpy(cameraUboMapped_, &ubo, sizeof(ubo));
    vmaFlushAllocation(allocator_, cameraUboAlloc_, 0, sizeof(ubo));
}

// ---- 渲染一帧 -----------------------------------------------------------
void VulkanEngine::drawFrame() {
    vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &inFlightFences_[currentFrame_]);

    uint32_t imageIndex;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imageAvailableSem_[currentFrame_],
                                       VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("vkAcquireNextImageKHR 失败");
    if (framebufferResized_) { framebufferResized_ = false; recreateSwapchain(); return; }

    updateCameraUbo();

    vkResetCommandBuffer(cmdBuffers_[currentFrame_], 0);
    recordCommandBuffer(cmdBuffers_[currentFrame_], imageIndex);

    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    VkCommandBufferSubmitInfo csi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    csi.commandBuffer = cmdBuffers_[currentFrame_];
    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = imageAvailableSem_[currentFrame_];
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    // renderFinished 按交换链图像索引，避免 MAILBOX 下信号量复用冲突
    signalInfo.semaphore = renderFinishedSem_[imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &csi;
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &waitInfo;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signalInfo;
    if (vkQueueSubmit2(graphicsQueue_, 1, &si, inFlightFences_[currentFrame_]) != VK_SUCCESS)
        throw std::runtime_error("vkQueueSubmit2 失败");
    // 等待本帧 GPU 完成，以便转储时主机读回
    if (dumpPending_) vkWaitForFences(device_, 1, &inFlightFences_[currentFrame_], VK_TRUE, UINT64_MAX);

    // 自动验证：present 之前转储交换链图像（读回后图像回到 PRESENT_SRC）
    if (dumpPending_) {
        dumpPending_ = false;
        dumpSwapchainImage(imageIndex);
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderFinishedSem_[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(presentQueue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreateSwapchain();

    currentFrame_ = (currentFrame_ + 1) % maxFramesInFlight_;
}

// ---- 录制命令缓冲区 -----------------------------------------------------
void VulkanEngine::recordCommandBuffer(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = 0;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS)
        throw std::runtime_error("vkBeginCommandBuffer 失败");

    // 交换链图像：UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL
    transitionImageLayout(cmd, swapchainImages_[imageIndex], swapchainFormat_,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          1, 1, VK_PIPELINE_STAGE_2_NONE,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_2_NONE, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    // 深度：UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.srcAccessMask = VK_ACCESS_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = depthImage_;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    }

    // 动态渲染
    VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAtt.imageView = swapchainImageViews_[imageIndex];
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue.color = {{0.05f, 0.06f, 0.08f, 1.0f}};
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depthImageView_;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, swapchainExtent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAtt;
    ri.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(cmd, &ri);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport vp{0, 0, (float)swapchainExtent_.width, (float)swapchainExtent_.height, 0, 1};
    VkRect2D sc{{0, 0}, swapchainExtent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    // 绑定几何
    VkBuffer vbs[] = {vertexBuffer_, instanceBuffer_};
    VkDeviceSize offsets[] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offsets);
    vkCmdBindIndexBuffer(cmd, indexBuffer_, 0, VK_INDEX_TYPE_UINT32);

    // Push Descriptors：set0 的 4 个 binding
    // binding0: camera UBO
    VkDescriptorBufferInfo camInfo{};
    camInfo.buffer = cameraUbo_;
    camInfo.offset = 0;
    camInfo.range = sizeof(CameraUbo);
    // binding1/2/3: 纹理数组
    std::vector<VkDescriptorImageInfo> baseInfos(cfg_->materials.size());
    std::vector<VkDescriptorImageInfo> roughInfos(cfg_->materials.size());
    std::vector<VkDescriptorImageInfo> normInfos(cfg_->materials.size());
    for (size_t i = 0; i < cfg_->materials.size(); ++i) {
        baseInfos[i] = {sampler_, baseColorTex_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        roughInfos[i] = {sampler_, roughnessTex_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        normInfos[i] = {sampler_, normalTex_[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    std::array<VkWriteDescriptorSet, 4> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = 0;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &camInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = 0;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = static_cast<uint32_t>(baseInfos.size());
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = baseInfos.data();
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = 0;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = static_cast<uint32_t>(roughInfos.size());
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = roughInfos.data();
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = 0;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = static_cast<uint32_t>(normInfos.size());
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = normInfos.data();
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                              0, static_cast<uint32_t>(writes.size()), writes.data());

    // 逐实例绘制（每实例一个 push constant）
    for (size_t i = 0; i < instances_.size(); ++i) {
        const Instance& inst = instances_[i];
        // push constant: mat4(64) + uint(4) + pad(12) + MaterialParams(32) = 112
        struct PC {
            float model[16];
            uint32_t matIdx;
            uint32_t pad[3];
            MaterialParams mp;
        } pc;
        std::memcpy(pc.model, inst.model, sizeof(float) * 16);
        pc.matIdx = inst.matIdx;
        pc.pad[0] = pc.pad[1] = pc.pad[2] = 0;
        pc.mp = matParams_[inst.matIdx];
        vkCmdPushConstants(cmd, pipelineLayout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(PC), &pc);
        vkCmdDrawIndexed(cmd, static_cast<uint32_t>(cubeIndices_.size()), 1, 0, 0, 0);
    }

    vkCmdEndRendering(cmd);

    // 自动验证：标记本帧需转储（在 drawFrame 提交等待后单独做主机读回）
    if (dumpFrame_ && !frameDumped_ && ++frameCounter_ >= 30) {
        frameDumped_ = true;
        dumpImageIndex_ = imageIndex;
        dumpPending_ = true;
    }

    // COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC_KHR
    transitionImageLayout(cmd, swapchainImages_[imageIndex], swapchainFormat_,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1, 1,
                          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                          VK_ACCESS_2_NONE);

    vkEndCommandBuffer(cmd);
}

// ---- 自动验证：把交换链图像读回并写 PPM ---------------------------------
// 交换链图像不支持 HOST_TRANSFER，故：vkCmdCopyImage 从交换链图像拷到临时图像
// （带 HOST_TRANSFER），再用 vkCopyImageToMemory 从临时图像读回主机。
void VulkanEngine::dumpSwapchainImage(uint32_t imageIndex) {
    VkImage srcImg = swapchainImages_[imageIndex];
    uint32_t w = swapchainExtent_.width, h = swapchainExtent_.height;

    // 创建临时可读回图像（与交换链同格式，TRANSFER_DST|HOST_TRANSFER）
    VkImage tmpImg; VmaAllocation tmpAlloc;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = swapchainFormat_;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{}; aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(allocator_, &ici, &aci, &tmpImg, &tmpAlloc, nullptr) != VK_SUCCESS) {
        std::fprintf(stderr, "dumpSwapchainImage: 创建临时图像失败\n"); return;
    }

    // 一个命令：交换链 PRESENT_SRC -> TRANSFER_SRC_OPTIMAL；
    // 临时 UNDEFINED -> TRANSFER_DST_OPTIMAL；vkCmdCopyImage；
    // 临时 -> GENERAL（host 读回）
    VkCommandBuffer cmd = beginOneTimeGraphics();
    transitionImageLayout(cmd, srcImg, swapchainFormat_, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1,
                          VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_READ_BIT);
    transitionImageLayout(cmd, tmpImg, swapchainFormat_, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, 1,
                          VK_PIPELINE_STAGE_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy2 copy{VK_STRUCTURE_TYPE_IMAGE_COPY_2};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = {w, h, 1};
    VkCopyImageInfo2 cii{VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
    cii.srcImage = srcImg;
    cii.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    cii.dstImage = tmpImg;
    cii.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    cii.regionCount = 1;
    cii.pRegions = &copy;
    vkCmdCopyImage2(cmd, &cii);
    transitionImageLayout(cmd, tmpImg, swapchainFormat_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_GENERAL, 1, 1,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_ACCESS_2_HOST_READ_BIT);
    // 交换链转回 PRESENT_SRC
    transitionImageLayout(cmd, srcImg, swapchainFormat_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 1, 1,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                          VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_NONE);
    endOneTimeGraphics(cmd);

    // 主机读回临时图像
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    VkImageToMemoryCopy region{VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY};
    region.pHostPointer = pixels.data();
    region.memoryRowLength = 0; region.memoryImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};
    VkCopyImageToMemoryInfo info{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO};
    info.srcImage = tmpImg;
    info.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    info.regionCount = 1;
    info.pRegions = &region;
    if (vkCopyImageToMemory(device_, &info) != VK_SUCCESS) {
        std::fprintf(stderr, "dumpSwapchainImage: vkCopyImageToMemory 失败\n");
        vmaDestroyImage(allocator_, tmpImg, tmpAlloc);
        return;
    }
    vmaDestroyImage(allocator_, tmpImg, tmpAlloc);

    // 交换链格式可能是 B8G8R8A8_SRGB，转成 R8G8B8 写 PPM
    const bool isBGRA = (swapchainFormat_ == VK_FORMAT_B8G8R8A8_SRGB ||
                         swapchainFormat_ == VK_FORMAT_B8G8R8A8_UNORM);
    std::FILE* f = std::fopen("rubik_frame.ppm", "wb");
    if (!f) { std::fprintf(stderr, "无法写 rubik_frame.ppm\n"); return; }
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            uint8_t r = pixels[idx + 0], g = pixels[idx + 1], b = pixels[idx + 2];
            if (isBGRA) { uint8_t t = r; r = b; b = t; }
            std::fputc(r, f); std::fputc(g, f); std::fputc(b, f);
        }
    }
    std::fclose(f);
    std::fprintf(stderr, "已转储交换链图像到 rubik_frame.ppm (%ux%u)\n", w, h);
}

} // namespace rubik
