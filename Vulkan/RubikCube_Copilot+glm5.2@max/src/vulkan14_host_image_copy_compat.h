#pragma once
// 兼容层：记录当前系统 Vulkan headers（1.4.357 过渡版本）对 Host Image Copy 的
// 实际支持情况。
//
// 重要事实（经 vk.xml 1.4 核心规范确认）：
//   - Vulkan 1.4 核心 **没有** VK_IMAGE_USAGE_HOST_IMAGE_COPY_BIT 这个 usage 位。
//     host image copy 对图像 usage 的真实要求是 VK_IMAGE_USAGE_HOST_TRANSFER_BIT
//     （VUID-VkCopyMemoryToImageInfo-dstImage-09113），配合启用 hostImageCopy 特性。
//   - Vulkan 1.4 核心 **没有** VK_IMAGE_LAYOUT_HOST_IMAGE_COPY_OPTIMAL 这个布局。
//     vkCopyMemoryToImage 的 dstImageLayout 只能用 VK_IMAGE_LAYOUT_GENERAL。
//   - Vulkan 1.4 核心 **没有** VK_ACCESS_2_HOST_IMAGE_COPY_*_BIT 这些访问位。
//     主机写入用通用的 VK_ACCESS_2_HOST_WRITE_BIT，GPU 读取用 VK_ACCESS_2_SHADER_READ_BIT。
//
// 此头文件目前无需定义任何宏/常量，仅作规范备注。

#include <vulkan/vulkan_core.h>
