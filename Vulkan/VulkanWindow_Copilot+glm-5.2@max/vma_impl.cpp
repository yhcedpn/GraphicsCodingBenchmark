// VMA（Vulkan Memory Allocator）实现编译单元。
//
// 约定：
// - volk.h 必须在本文件之前包含，以便 VMA 启用 volk 专用的函数表导入
//   （vmaImportVulkanFunctionsFromVolk 仅在 “volk.h 已先于 vk_mem_alloc.h 包含” 时声明）。
// - VMA_IMPLEMENTATION 只能定义于全局唯一一个翻译单元；本文件即该单元。
// - 本文件单独编译并关闭严格警告（VMA 实现体量较大，会触发 -Wpedantic 等）。
#define VK_NO_PROTOTYPES
#include <volk.h>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
