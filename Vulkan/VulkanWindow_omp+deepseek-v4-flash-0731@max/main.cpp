// VulkanWindow：GLFW + Vulkan 渲染四象限纯色窗口。
// 布局：左上=红(1,0,0)，左下=白(1,1,1)，右下=蓝(0,0,1)，右上=绿(0,1,0)，
// 分界线位于水平与垂直中心；随窗口尺寸变化自动重建交换链，关闭窗口后正常退出。
//
// 依赖：volk（动态加载 Vulkan）、GLFW（窗口/事件/Surface）、
//       预编译 SPIR-V（shaders.h，由 shaders/ 下 GLSL 经 glslc 生成）。
#include <volk.h>
#include <GLFW/glfw3.h>
#include "shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// vcpkg 提供的导入库（MSBuild 集成已配置 vcpkg 库搜索路径）。
#pragma comment(lib, "volk.lib")
#pragma comment(lib, "glfw3dll.lib")

namespace {

// 检查 Vulkan 结果，失败时打印原因并退出。
void Check(VkResult res, const char* what) {
    if (res != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan 错误: %s -> %d\n", what, static_cast<int>(res));
        std::exit(1);
    }
}

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "错误: %s\n", what);
        std::exit(1);
    }
}

constexpr int kMaxFramesInFlight = 2;

struct App {
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t graphicsFamily = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> swapchainFramebuffers;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers[kMaxFramesInFlight]{};
    VkSemaphore imageAvailable[kMaxFramesInFlight]{};
    VkSemaphore renderFinished[kMaxFramesInFlight]{};
    VkFence inFlightFence[kMaxFramesInFlight]{};

    bool framebufferResized = false;
    uint32_t frameIndex = 0;
};

// 调试回调：仅在启用校验层时输出告警/错误。
static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT /*severity*/,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*user*/) {
    std::fprintf(stderr, "[校验层] %s\n", data->pMessage ? data->pMessage : "");
    return VK_FALSE;
}

void CreateInstance(App& app) {
    uint32_t glfwExtCount = 0;
    const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
    Check(glfwExts != nullptr, "GLFW 无法提供 Vulkan 实例扩展（驱动不支持 Vulkan？）");

    // 查询可用的实例扩展与校验层（全部可选）。
    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> instExts(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, instExts.data());
    bool hasDebugUtils = false;
    for (const auto& e : instExts)
        if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) hasDebugUtils = true;

    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool hasValidation = false;
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) hasValidation = true;

    std::vector<const char*> extensions(glfwExts, glfwExts + glfwExtCount);
    if (hasDebugUtils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const char* layerName = "VK_LAYER_KHRONOS_validation";

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VulkanWindow";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    if (hasValidation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layerName;
    }
    Check(vkCreateInstance(&ci, nullptr, &app.instance) == VK_SUCCESS, "vkCreateInstance 失败");
    volkLoadInstance(app.instance);

    if (hasDebugUtils) {
        VkDebugUtilsMessengerCreateInfoEXT dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = DebugCallback;
        vkCreateDebugUtilsMessengerEXT(app.instance, &dci, nullptr, &app.debugMessenger);
    }
}

void CreateDevice(App& app) {
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(app.instance, &deviceCount, nullptr);
    Check(deviceCount > 0, "未找到 Vulkan 物理设备");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(app.instance, &deviceCount, devices.data());

    // 选择第一个同时支持图形队列与表面呈现的设备。
    for (VkPhysicalDevice pd : devices) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> qps(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qCount, qps.data());
        for (uint32_t i = 0; i < qCount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, app.surface, &present);
            if ((qps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present) {
                app.physicalDevice = pd;
                app.graphicsFamily = i;
                break;
            }
        }
        if (app.physicalDevice != VK_NULL_HANDLE) break;
    }
    Check(app.physicalDevice != VK_NULL_HANDLE, "没有同时支持图形与呈现的队列族");

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(app.physicalDevice, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> devExts(extCount);
    vkEnumerateDeviceExtensionProperties(app.physicalDevice, nullptr, &extCount, devExts.data());
    bool hasSwapchain = false;
    for (const auto& e : devExts)
        if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) hasSwapchain = true;
    Check(hasSwapchain, "物理设备不支持交换链扩展");

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = app.graphicsFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* deviceExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &deviceExt;
    Check(vkCreateDevice(app.physicalDevice, &dci, nullptr, &app.device) == VK_SUCCESS,
          "vkCreateDevice 失败");
    volkLoadDevice(app.device);
    vkGetDeviceQueue(app.device, app.graphicsFamily, 0, &app.graphicsQueue);
    app.presentQueue = app.graphicsQueue;  // 同一队列族，呈现与图形共用

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(app.physicalDevice, &props);
    std::printf("设备: %s\n", props.deviceName);
}

// 选择交换链格式：优先 B8G8R8A8_UNORM（非 sRGB，保证颜色精确呈现）。
VkSurfaceFormatKHR PickFormat(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &n, formats.data());
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    if (!formats.empty()) return formats[0];
    return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
}

// 呈现模式：优先 MAILBOX（低延迟），否则回退 FIFO（始终可用，垂直同步）。
VkPresentModeKHR PickPresentMode(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &n, modes.data());
    for (VkPresentModeKHR m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D PickExtent(const VkSurfaceCapabilitiesKHR& caps, GLFWwindow* window) {
    if (caps.currentExtent.width != 0xFFFFFFFFu) return caps.currentExtent;
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    VkExtent2D e{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    e.width = std::clamp(e.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    e.height = std::clamp(e.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return e;
}

void CreateSwapchain(App& app) {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(app.physicalDevice, app.surface, &caps);
    VkSurfaceFormatKHR format = PickFormat(app.physicalDevice, app.surface);
    VkPresentModeKHR mode = PickPresentMode(app.physicalDevice, app.surface);
    VkExtent2D extent = PickExtent(caps, app.window);
    Check(extent.width >= 1 && extent.height >= 1, "窗口尺寸无效（最小化？）");

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = app.surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    Check(vkCreateSwapchainKHR(app.device, &ci, nullptr, &app.swapchain) == VK_SUCCESS,
          "vkCreateSwapchainKHR 失败");

    app.swapchainFormat = format.format;
    app.swapchainExtent = extent;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(app.device, app.swapchain, &n, nullptr);
    app.swapchainImages.resize(n);
    vkGetSwapchainImagesKHR(app.device, app.swapchain, &n, app.swapchainImages.data());

    app.swapchainImageViews.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = app.swapchainImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = app.swapchainFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(vkCreateImageView(app.device, &vci, nullptr, &app.swapchainImageViews[i]) ==
                  VK_SUCCESS,
              "vkCreateImageView 失败");
    }
    std::printf("交换链: %dx%d, 图像 %u, 呈现模式 %d\n", extent.width, extent.height, n,
                static_cast<int>(mode));
}

void CreateRenderPass(App& app) {
    VkAttachmentDescription att{};
    att.format = app.swapchainFormat;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // 全屏三角形会覆盖全部像素，清屏作为兜底
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    // 等待呈现引擎写完当前帧后，再执行颜色附件写入。
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rci{};
    rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rci.attachmentCount = 1;
    rci.pAttachments = &att;
    rci.subpassCount = 1;
    rci.pSubpasses = &subpass;
    rci.dependencyCount = 1;
    rci.pDependencies = &dep;
    Check(vkCreateRenderPass(app.device, &rci, nullptr, &app.renderPass) == VK_SUCCESS,
          "vkCreateRenderPass 失败");
}

VkShaderModule CreateShaderModule(App& app, const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(app.device, &ci, nullptr, &module) == VK_SUCCESS,
          "vkCreateShaderModule 失败");
    return module;
}

void CreatePipeline(App& app) {
    VkShaderModule vert = CreateShaderModule(app, kVertexSpv, sizeof(kVertexSpv));
    VkShaderModule frag = CreateShaderModule(app, kFragmentSpv, sizeof(kFragmentSpv));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;

    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;  // 无顶点输入

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // 视口与裁剪框使用动态状态，窗口尺寸变化时无需重建管线。
    VkPipelineViewportStateCreateInfo vs{};
    vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vs.viewportCount = 1;
    vs.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamicStates;

    // 片段着色器的分辨率（vec2，8 字节）通过推送常量传入。
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = 8;

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    Check(vkCreatePipelineLayout(app.device, &pl, nullptr, &app.pipelineLayout) == VK_SUCCESS,
          "vkCreatePipelineLayout 失败");

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vs;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = app.pipelineLayout;
    gpi.renderPass = app.renderPass;
    gpi.subpass = 0;
    Check(vkCreateGraphicsPipelines(app.device, VK_NULL_HANDLE, 1, &gpi, nullptr,
                                    &app.pipeline) == VK_SUCCESS,
          "vkCreateGraphicsPipelines 失败");

    vkDestroyShaderModule(app.device, vert, nullptr);
    vkDestroyShaderModule(app.device, frag, nullptr);
}

void CreateFramebuffers(App& app) {
    app.swapchainFramebuffers.resize(app.swapchainImageViews.size());
    for (size_t i = 0; i < app.swapchainImageViews.size(); ++i) {
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = app.renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &app.swapchainImageViews[i];
        fci.width = app.swapchainExtent.width;
        fci.height = app.swapchainExtent.height;
        fci.layers = 1;
        Check(vkCreateFramebuffer(app.device, &fci, nullptr, &app.swapchainFramebuffers[i]) ==
                  VK_SUCCESS,
              "vkCreateFramebuffer 失败");
    }
}

void CreateCommandPool(App& app) {
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = app.graphicsFamily;
    Check(vkCreateCommandPool(app.device, &cpi, nullptr, &app.commandPool) == VK_SUCCESS,
          "vkCreateCommandPool 失败");

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = app.commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kMaxFramesInFlight;
    Check(vkAllocateCommandBuffers(app.device, &ai, app.commandBuffers) == VK_SUCCESS,
          "vkAllocateCommandBuffers 失败");
}

void CreateSyncObjects(App& app) {
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // 首帧可直接等待
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        Check(vkCreateSemaphore(app.device, &sci, nullptr, &app.imageAvailable[i]) ==
                  VK_SUCCESS,
              "vkCreateSemaphore 失败");
        Check(vkCreateSemaphore(app.device, &sci, nullptr, &app.renderFinished[i]) ==
                  VK_SUCCESS,
              "vkCreateSemaphore 失败");
        Check(vkCreateFence(app.device, &fci, nullptr, &app.inFlightFence[i]) == VK_SUCCESS,
              "vkCreateFence 失败");
    }
}

void RecordCommandBuffer(App& app, uint32_t imageIndex, uint32_t frame) {
    VkCommandBuffer cb = app.commandBuffers[frame];
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    Check(vkBeginCommandBuffer(cb, &bi) == VK_SUCCESS, "vkBeginCommandBuffer 失败");

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = app.renderPass;
    rbi.framebuffer = app.swapchainFramebuffers[imageIndex];
    rbi.renderArea = {{0, 0}, app.swapchainExtent};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, app.pipeline);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(app.swapchainExtent.width),
                        static_cast<float>(app.swapchainExtent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, app.swapchainExtent};
    vkCmdSetScissor(cb, 0, 1, &scissor);

    const float resolution[2] = {static_cast<float>(app.swapchainExtent.width),
                                 static_cast<float>(app.swapchainExtent.height)};
    vkCmdPushConstants(cb, app.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(resolution), resolution);

    vkCmdDraw(cb, 3, 1, 0, 0);  // 全屏三角形
    vkCmdEndRenderPass(cb);
    Check(vkEndCommandBuffer(cb) == VK_SUCCESS, "vkEndCommandBuffer 失败");
}

// 交换链失效或窗口尺寸变化时重建；最小化期间等待恢复。
void RecreateSwapchain(App& app) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(app.window, &w, &h);
    while (w == 0 || h == 0) {
        glfwWaitEvents();
        glfwGetFramebufferSize(app.window, &w, &h);
    }
    vkDeviceWaitIdle(app.device);
    for (VkFramebuffer fb : app.swapchainFramebuffers) vkDestroyFramebuffer(app.device, fb, nullptr);
    for (VkImageView v : app.swapchainImageViews) vkDestroyImageView(app.device, v, nullptr);
    vkDestroySwapchainKHR(app.device, app.swapchain, nullptr);
    app.swapchainFramebuffers.clear();
    app.swapchainImageViews.clear();
    CreateSwapchain(app);
    CreateFramebuffers(app);
    app.framebufferResized = false;
}

void RenderFrame(App& app) {
    const uint32_t frame = app.frameIndex % kMaxFramesInFlight;
    vkWaitForFences(app.device, 1, &app.inFlightFence[frame], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult res = vkAcquireNextImageKHR(app.device, app.swapchain, UINT64_MAX,
                                         app.imageAvailable[frame], VK_NULL_HANDLE, &imageIndex);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain(app);
        return;
    }
    Check(res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR, "vkAcquireNextImageKHR 失败");

    vkResetFences(app.device, 1, &app.inFlightFence[frame]);
    vkResetCommandBuffer(app.commandBuffers[frame], 0);
    RecordCommandBuffer(app, imageIndex, frame);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &app.imageAvailable[frame];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &app.commandBuffers[frame];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &app.renderFinished[frame];
    Check(vkQueueSubmit(app.graphicsQueue, 1, &si, app.inFlightFence[frame]) == VK_SUCCESS,
          "vkQueueSubmit 失败");

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &app.renderFinished[frame];
    pi.swapchainCount = 1;
    pi.pSwapchains = &app.swapchain;
    pi.pImageIndices = &imageIndex;
    res = vkQueuePresentKHR(app.presentQueue, &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        RecreateSwapchain(app);
        return;
    }
    Check(res == VK_SUCCESS, "vkQueuePresentKHR 失败");
    ++app.frameIndex;
}

void Cleanup(App& app) {
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        vkDestroyFence(app.device, app.inFlightFence[i], nullptr);
        vkDestroySemaphore(app.device, app.renderFinished[i], nullptr);
        vkDestroySemaphore(app.device, app.imageAvailable[i], nullptr);
    }
    vkDestroyCommandPool(app.device, app.commandPool, nullptr);
    vkDestroyPipeline(app.device, app.pipeline, nullptr);
    vkDestroyPipelineLayout(app.device, app.pipelineLayout, nullptr);
    vkDestroyRenderPass(app.device, app.renderPass, nullptr);
    for (VkFramebuffer fb : app.swapchainFramebuffers) vkDestroyFramebuffer(app.device, fb, nullptr);
    for (VkImageView v : app.swapchainImageViews) vkDestroyImageView(app.device, v, nullptr);
    vkDestroySwapchainKHR(app.device, app.swapchain, nullptr);
    vkDestroyDevice(app.device, nullptr);
    vkDestroySurfaceKHR(app.instance, app.surface, nullptr);
    if (app.debugMessenger != VK_NULL_HANDLE)
        vkDestroyDebugUtilsMessengerEXT(app.instance, app.debugMessenger, nullptr);
    vkDestroyInstance(app.instance, nullptr);
    glfwDestroyWindow(app.window);
    glfwTerminate();
}

int Run() {
    Check(volkInitialize() == VK_SUCCESS, "volkInitialize 失败（无法加载 vulkan-1.dll）");
    Check(glfwInit(), "glfwInit 失败");
    glfwInitVulkanLoader(vkGetInstanceProcAddr);  // 让 GLFW 通过 volk 解析 Vulkan 函数

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    App app;
    app.window = glfwCreateWindow(800, 600, "VulkanWindow", nullptr, nullptr);
    Check(app.window != nullptr, "glfwCreateWindow 失败");
    glfwSetWindowUserPointer(app.window, &app);
    glfwSetFramebufferSizeCallback(app.window, [](GLFWwindow* w, int, int) {
        static_cast<App*>(glfwGetWindowUserPointer(w))->framebufferResized = true;
    });

    CreateInstance(app);
    Check(glfwCreateWindowSurface(app.instance, app.window, nullptr, &app.surface) == VK_SUCCESS,
          "glfwCreateWindowSurface 失败");
    CreateDevice(app);
    CreateSwapchain(app);
    CreateRenderPass(app);
    CreatePipeline(app);
    CreateFramebuffers(app);
    CreateCommandPool(app);
    CreateSyncObjects(app);
    std::printf("窗口就绪\n");
    std::fflush(stdout);

    while (!glfwWindowShouldClose(app.window)) {
        glfwPollEvents();
        if (app.framebufferResized) {
            RecreateSwapchain(app);
            continue;
        }
        if (glfwGetWindowAttrib(app.window, GLFW_ICONIFIED)) {
            glfwWaitEvents();  // 最小化时暂停渲染，等待恢复事件
            continue;
        }
        RenderFrame(app);
    }

    vkDeviceWaitIdle(app.device);
    Cleanup(app);
    std::printf("已退出\n");
    return 0;
}

}  // namespace

int main() {
    return Run();
}
