#include <volk.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t WindowWidth = 800;
constexpr std::uint32_t WindowHeight = 600;
constexpr std::size_t MaxFramesInFlight = 2;

void checkVulkan(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " 失败，VkResult=" + std::to_string(result));
    }
}

struct QueueFamilyIndices {
    std::optional<std::uint32_t> graphics;
    std::optional<std::uint32_t> present;

    [[nodiscard]] bool complete() const {
        return graphics.has_value() && present.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

class VulkanWindow {
public:
    VulkanWindow() {
        try {
            initWindow();
            initVulkan();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~VulkanWindow() {
        cleanup();
    }

    VulkanWindow(const VulkanWindow&) = delete;
    VulkanWindow& operator=(const VulkanWindow&) = delete;

    void run() {
        while (!glfwWindowShouldClose(window_)) {
            glfwPollEvents();
            drawFrame();
        }

        if (device_ != VK_NULL_HANDLE) {
            checkVulkan(vkDeviceWaitIdle(device_), "等待设备空闲");
        }
    }

private:
    void initWindow() {
        if (glfwInit() == GLFW_FALSE) {
            throw std::runtime_error("初始化 GLFW 失败");
        }
        glfwInitialized_ = true;

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window_ = glfwCreateWindow(
            static_cast<int>(WindowWidth),
            static_cast<int>(WindowHeight),
            "Vulkan 四象限",
            nullptr,
            nullptr);
        if (window_ == nullptr) {
            throw std::runtime_error("创建 GLFW 窗口失败");
        }
        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);
    }

    void initVulkan() {
        checkVulkan(volkInitialize(), "初始化 volk");
        volkInitialized_ = true;

        createInstance();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createCommandPool();
        createCommandBuffers();
        createSyncObjects();
        createSwapChainResources();
    }

    void createInstance() {
        if (glfwVulkanSupported() == GLFW_FALSE) {
            throw std::runtime_error("当前系统不支持 Vulkan");
        }

        VkApplicationInfo applicationInfo{};
        applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        applicationInfo.pApplicationName = "Vulkan 四象限";
        applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.pEngineName = "RenderArena";
        applicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.apiVersion = VK_API_VERSION_1_0;

        std::uint32_t extensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        if (glfwExtensions == nullptr) {
            throw std::runtime_error("获取 GLFW Vulkan 扩展失败");
        }

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &applicationInfo;
        createInfo.enabledExtensionCount = extensionCount;
        createInfo.ppEnabledExtensionNames = glfwExtensions;

        checkVulkan(vkCreateInstance(&createInfo, nullptr, &instance_), "创建 Vulkan 实例");
        volkLoadInstance(instance_);
    }

    void createSurface() {
        checkVulkan(glfwCreateWindowSurface(instance_, window_, nullptr, &surface_), "创建 Vulkan Surface");
    }

    void pickPhysicalDevice() {
        std::uint32_t deviceCount = 0;
        checkVulkan(vkEnumeratePhysicalDevices(instance_, &deviceCount, nullptr), "枚举物理设备数量");
        if (deviceCount == 0) {
            throw std::runtime_error("没有可用的 Vulkan 物理设备");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        checkVulkan(vkEnumeratePhysicalDevices(instance_, &deviceCount, devices.data()), "枚举物理设备");
        for (VkPhysicalDevice device : devices) {
            if (isDeviceSuitable(device)) {
                physicalDevice_ = device;
                queueFamilies_ = findQueueFamilies(device);
                break;
            }
        }

        if (physicalDevice_ == VK_NULL_HANDLE) {
            throw std::runtime_error("没有支持窗口呈现的 Vulkan 物理设备");
        }
    }

    void createLogicalDevice() {
        std::set<std::uint32_t> uniqueQueueFamilies{
            queueFamilies_.graphics.value(),
            queueFamilies_.present.value()};

        const float queuePriority = 1.0F;
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        queueCreateInfos.reserve(uniqueQueueFamilies.size());
        for (std::uint32_t queueFamily : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        VkPhysicalDeviceFeatures deviceFeatures{};
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions_.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions_.data();

        checkVulkan(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "创建逻辑设备");
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, queueFamilies_.graphics.value(), 0, &graphicsQueue_);
        vkGetDeviceQueue(device_, queueFamilies_.present.value(), 0, &presentQueue_);
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilies_.graphics.value();
        checkVulkan(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_), "创建命令池");
    }

    void createCommandBuffers() {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = commandPool_;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = static_cast<std::uint32_t>(commandBuffers_.size());
        checkVulkan(vkAllocateCommandBuffers(device_, &allocateInfo, commandBuffers_.data()), "分配命令缓冲区");
    }

    void createSyncObjects() {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (std::size_t i = 0; i < MaxFramesInFlight; ++i) {
            checkVulkan(
                vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]),
                "创建图像可用信号量");
            checkVulkan(
                vkCreateSemaphore(device_, &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]),
                "创建渲染完成信号量");
            checkVulkan(vkCreateFence(device_, &fenceInfo, nullptr, &inFlightFences_[i]), "创建帧栅栏");
        }
    }

    void createSwapChainResources() {
        createSwapChain();
        createImageViews();
        createRenderPass();
        createFramebuffers();
    }

    void createSwapChain() {
        SwapChainSupportDetails support = querySwapChainSupport(physicalDevice_);
        const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(support.formats);
        const VkPresentModeKHR presentMode = choosePresentMode(support.presentModes);
        const VkExtent2D extent = chooseSwapExtent(support.capabilities);

        std::uint32_t imageCount = support.capabilities.minImageCount + 1;
        if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
            imageCount = support.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface_;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

        const std::array<std::uint32_t, 2> queueFamilyIndices{
            queueFamilies_.graphics.value(),
            queueFamilies_.present.value()};
        if (queueFamilies_.graphics != queueFamilies_.present) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(queueFamilyIndices.size());
            createInfo.pQueueFamilyIndices = queueFamilyIndices.data();
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = support.capabilities.currentTransform;
        createInfo.compositeAlpha = chooseCompositeAlpha(support.capabilities.supportedCompositeAlpha);
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;

        checkVulkan(vkCreateSwapchainKHR(device_, &createInfo, nullptr, &swapchain_), "创建交换链");
        swapchainImageFormat_ = surfaceFormat.format;
        swapchainExtent_ = extent;

        std::uint32_t actualImageCount = 0;
        checkVulkan(vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, nullptr), "获取交换链图像数量");
        swapchainImages_.resize(actualImageCount);
        checkVulkan(
            vkGetSwapchainImagesKHR(device_, swapchain_, &actualImageCount, swapchainImages_.data()),
            "获取交换链图像");
        imagesInFlight_.assign(actualImageCount, VK_NULL_HANDLE);
    }

    void createImageViews() {
        swapchainImageViews_.resize(swapchainImages_.size());
        for (std::size_t i = 0; i < swapchainImages_.size(); ++i) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = swapchainImages_[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = swapchainImageFormat_;
            viewInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;

            checkVulkan(vkCreateImageView(device_, &viewInfo, nullptr, &swapchainImageViews_[i]), "创建交换链图像视图");
        }
    }

    void createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapchainImageFormat_;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorAttachmentReference{};
        colorAttachmentReference.attachment = 0;
        colorAttachmentReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentReference;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        checkVulkan(vkCreateRenderPass(device_, &renderPassInfo, nullptr, &renderPass_), "创建渲染通道");
    }

    void createFramebuffers() {
        swapchainFramebuffers_.resize(swapchainImageViews_.size());
        for (std::size_t i = 0; i < swapchainImageViews_.size(); ++i) {
            VkImageView attachments[] = {swapchainImageViews_[i]};
            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass_;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = attachments;
            framebufferInfo.width = swapchainExtent_.width;
            framebufferInfo.height = swapchainExtent_.height;
            framebufferInfo.layers = 1;

            checkVulkan(
                vkCreateFramebuffer(device_, &framebufferInfo, nullptr, &swapchainFramebuffers_[i]),
                "创建帧缓冲区");
        }
    }

    void drawFrame() {
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
        if (framebufferWidth == 0 || framebufferHeight == 0) {
            glfwWaitEvents();
            return;
        }

        VkFence currentFence = inFlightFences_[currentFrame_];
        checkVulkan(vkWaitForFences(device_, 1, &currentFence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "等待帧栅栏");

        std::uint32_t imageIndex = 0;
        VkResult acquireResult = vkAcquireNextImageKHR(
            device_,
            swapchain_,
            std::numeric_limits<std::uint64_t>::max(),
            imageAvailableSemaphores_[currentFrame_],
            VK_NULL_HANDLE,
            &imageIndex);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapChain();
            return;
        }
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
            checkVulkan(acquireResult, "获取交换链图像");
        }

        if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE) {
            VkFence imageFence = imagesInFlight_[imageIndex];
            checkVulkan(vkWaitForFences(device_, 1, &imageFence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "等待交换链图像栅栏");
        }
        imagesInFlight_[imageIndex] = currentFence;

        checkVulkan(vkResetFences(device_, 1, &currentFence), "重置帧栅栏");
        checkVulkan(vkResetCommandBuffer(commandBuffers_[currentFrame_], 0), "重置命令缓冲区");
        recordCommandBuffer(commandBuffers_[currentFrame_], imageIndex);

        VkSemaphore waitSemaphores[] = {imageAvailableSemaphores_[currentFrame_]};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        VkSemaphore signalSemaphores[] = {renderFinishedSemaphores_[currentFrame_]};

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        checkVulkan(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, currentFence), "提交绘制命令");

        VkSwapchainKHR swapchains[] = {swapchain_};
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &imageIndex;

        VkResult presentResult = vkQueuePresentKHR(presentQueue_, &presentInfo);
        if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR && presentResult != VK_ERROR_OUT_OF_DATE_KHR) {
            checkVulkan(presentResult, "呈现交换链图像");
        }

        if (acquireResult == VK_SUBOPTIMAL_KHR ||
            presentResult == VK_SUBOPTIMAL_KHR ||
            presentResult == VK_ERROR_OUT_OF_DATE_KHR ||
            framebufferResized_) {
            framebufferResized_ = false;
            recreateSwapChain();
        }

        currentFrame_ = (currentFrame_ + 1) % MaxFramesInFlight;
    }

    void recordCommandBuffer(VkCommandBuffer commandBuffer, std::uint32_t imageIndex) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        checkVulkan(vkBeginCommandBuffer(commandBuffer, &beginInfo), "开始记录命令缓冲区");

        VkClearValue initialClearValue{};
        initialClearValue.color.float32[0] = 0.0F;
        initialClearValue.color.float32[1] = 0.0F;
        initialClearValue.color.float32[2] = 0.0F;
        initialClearValue.color.float32[3] = 1.0F;

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass_;
        renderPassInfo.framebuffer = swapchainFramebuffers_[imageIndex];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = swapchainExtent_;
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &initialClearValue;
        const std::uint32_t halfWidth = swapchainExtent_.width / 2;
        const std::uint32_t halfHeight = swapchainExtent_.height / 2;
        const std::uint32_t rightWidth = swapchainExtent_.width - halfWidth;
        const std::uint32_t bottomHeight = swapchainExtent_.height - halfHeight;

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
        clearQuadrant(commandBuffer, {1, 0, 0, 1}, makeRect(0, 0, halfWidth, halfHeight));
        clearQuadrant(commandBuffer, {0, 1, 0, 1}, makeRect(halfWidth, 0, rightWidth, halfHeight));
        clearQuadrant(commandBuffer, {0, 0, 1, 1}, makeRect(halfWidth, halfHeight, rightWidth, bottomHeight));
        clearQuadrant(commandBuffer, {1, 1, 1, 1}, makeRect(0, halfHeight, halfWidth, bottomHeight));
        vkCmdEndRenderPass(commandBuffer);

        checkVulkan(vkEndCommandBuffer(commandBuffer), "结束记录命令缓冲区");
    }

    static VkRect2D makeRect(
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) {
        VkRect2D rectangle{};
        rectangle.offset.x = static_cast<std::int32_t>(x);
        rectangle.offset.y = static_cast<std::int32_t>(y);
        rectangle.extent.width = width;
        rectangle.extent.height = height;
        return rectangle;
    }

    static void clearQuadrant(
        VkCommandBuffer commandBuffer,
        const std::array<float, 4>& color,
        VkRect2D rectangle) {
        if (rectangle.extent.width == 0 || rectangle.extent.height == 0) {
            return;
        }

        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        attachment.clearValue.color.float32[0] = color[0];
        attachment.clearValue.color.float32[1] = color[1];
        attachment.clearValue.color.float32[2] = color[2];
        attachment.clearValue.color.float32[3] = color[3];

        VkClearRect clearRect{};
        clearRect.rect = rectangle;
        clearRect.baseArrayLayer = 0;
        clearRect.layerCount = 1;
        vkCmdClearAttachments(commandBuffer, 1, &attachment, 1, &clearRect);
    }

    void recreateSwapChain() {
        int framebufferWidth = 0;
        int framebufferHeight = 0;
        glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
        while ((framebufferWidth == 0 || framebufferHeight == 0) && !glfwWindowShouldClose(window_)) {
            glfwWaitEvents();
            glfwGetFramebufferSize(window_, &framebufferWidth, &framebufferHeight);
        }
        if (glfwWindowShouldClose(window_)) {
            return;
        }

        checkVulkan(vkDeviceWaitIdle(device_), "重建交换链前等待设备空闲");
        cleanupSwapChain();
        createSwapChainResources();
    }

    void cleanupSwapChain() {
        for (VkFramebuffer framebuffer : swapchainFramebuffers_) {
            if (framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device_, framebuffer, nullptr);
            }
        }
        swapchainFramebuffers_.clear();

        if (renderPass_ != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }

        for (VkImageView imageView : swapchainImageViews_) {
            if (imageView != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, imageView, nullptr);
            }
        }
        swapchainImageViews_.clear();

        if (swapchain_ != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        swapchainImages_.clear();
        imagesInFlight_.clear();
    }

    void cleanup() noexcept {
        if (device_ != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device_);
            cleanupSwapChain();
        }

        for (std::size_t i = 0; i < MaxFramesInFlight; ++i) {
            if (renderFinishedSemaphores_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, renderFinishedSemaphores_[i], nullptr);
            }
            if (imageAvailableSemaphores_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, imageAvailableSemaphores_[i], nullptr);
            }
            if (inFlightFences_[i] != VK_NULL_HANDLE) {
                vkDestroyFence(device_, inFlightFences_[i], nullptr);
            }
        }

        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_, commandPool_, nullptr);
        }
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        if (volkInitialized_) {
            volkFinalize();
            volkInitialized_ = false;
        }
        if (window_ != nullptr) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
        if (glfwInitialized_) {
            glfwTerminate();
            glfwInitialized_ = false;
        }
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const {
        QueueFamilyIndices indices;
        std::uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        for (std::uint32_t i = 0; i < queueFamilyCount; ++i) {
            if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                indices.graphics = i;
            }

            VkBool32 presentSupport = VK_FALSE;
            checkVulkan(vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &presentSupport), "查询队列呈现支持");
            if (presentSupport == VK_TRUE) {
                indices.present = i;
            }

            if (indices.complete()) {
                break;
            }
        }
        return indices;
    }

    bool isDeviceSuitable(VkPhysicalDevice device) const {
        QueueFamilyIndices indices = findQueueFamilies(device);
        if (!indices.complete() || !checkDeviceExtensionSupport(device)) {
            return false;
        }

        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
        return !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice device) const {
        std::uint32_t extensionCount = 0;
        checkVulkan(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr), "枚举设备扩展数量");
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        checkVulkan(
            vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data()),
            "枚举设备扩展");

        std::set<std::string> requiredExtensions(deviceExtensions_.begin(), deviceExtensions_.end());
        for (const VkExtensionProperties& extension : availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }
        return requiredExtensions.empty();
    }

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) const {
        SwapChainSupportDetails details;
        checkVulkan(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface_, &details.capabilities), "查询交换链能力");

        std::uint32_t formatCount = 0;
        checkVulkan(vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, nullptr), "查询表面格式数量");
        if (formatCount > 0) {
            details.formats.resize(formatCount);
            checkVulkan(
                vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formatCount, details.formats.data()),
                "查询表面格式");
        }

        std::uint32_t presentModeCount = 0;
        checkVulkan(
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, nullptr),
            "查询呈现模式数量");
        if (presentModeCount > 0) {
            details.presentModes.resize(presentModeCount);
            checkVulkan(
                vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &presentModeCount, details.presentModes.data()),
                "查询呈现模式");
        }
        return details;
    }

    static VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
        for (const VkSurfaceFormatKHR& format : formats) {
            if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        return formats.front();
    }

    static VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& presentModes) {
        for (VkPresentModeKHR presentMode : presentModes) {
            if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return presentMode;
            }
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            return capabilities.currentExtent;
        }

        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        VkExtent2D actualExtent{
            static_cast<std::uint32_t>(std::max(width, 0)),
            static_cast<std::uint32_t>(std::max(height, 0))};
        actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return actualExtent;
    }

    static VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
        constexpr std::array<VkCompositeAlphaFlagBitsKHR, 4> candidates{
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
        for (VkCompositeAlphaFlagBitsKHR candidate : candidates) {
            if ((supported & candidate) != 0) {
                return candidate;
            }
        }
        throw std::runtime_error("交换链没有可用的合成 Alpha 模式");
    }

    static void framebufferResizeCallback(GLFWwindow* window, int, int) {
        auto* application = static_cast<VulkanWindow*>(glfwGetWindowUserPointer(window));
        if (application != nullptr) {
            application->framebufferResized_ = true;
        }
    }

    GLFWwindow* window_ = nullptr;
    bool glfwInitialized_ = false;
    bool volkInitialized_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    QueueFamilyIndices queueFamilies_;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages_;
    VkFormat swapchainImageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImageView> swapchainImageViews_;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapchainFramebuffers_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, MaxFramesInFlight> commandBuffers_{};
    std::array<VkSemaphore, MaxFramesInFlight> imageAvailableSemaphores_{};
    std::array<VkSemaphore, MaxFramesInFlight> renderFinishedSemaphores_{};
    std::array<VkFence, MaxFramesInFlight> inFlightFences_{};
    std::vector<VkFence> imagesInFlight_;
    std::size_t currentFrame_ = 0;
    bool framebufferResized_ = false;

    const std::vector<const char*> deviceExtensions_{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
};

} // 匿名命名空间

int main() {
    try {
        VulkanWindow application;
        application.run();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}