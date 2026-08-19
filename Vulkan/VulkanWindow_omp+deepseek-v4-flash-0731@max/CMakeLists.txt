cmake_minimum_required(VERSION 3.28)

project(VulkanWindow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # 源码含简体中文 UTF-8 字符串字面量，统一按 UTF-8 解释。
    add_compile_options(/utf-8)
endif()

find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(VulkanUtilityLibraries CONFIG REQUIRED)
find_package(unofficial-shaderc CONFIG REQUIRED)
find_package(SPIRV-Tools CONFIG REQUIRED)
find_package(unofficial-spirv-reflect CONFIG REQUIRED)

add_executable(VulkanWindow main.cpp)
target_link_libraries(VulkanWindow PRIVATE
    Vulkan::Vulkan
    glfw
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    Vulkan::CompilerConfiguration
    Vulkan::UtilityHeaders
    unofficial::shaderc::shaderc
    SPIRV-Tools-static
    unofficial::spirv-reflect
)
