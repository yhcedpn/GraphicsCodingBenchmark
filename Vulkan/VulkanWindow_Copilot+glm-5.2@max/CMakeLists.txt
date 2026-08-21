cmake_minimum_required(VERSION 3.28)

project(VulkanWindow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(Vulkan REQUIRED)
if(TARGET Vulkan::Headers)
    set(VULKAN_HEADERS_TARGET Vulkan::Headers)
elseif(Vulkan_INCLUDE_DIR)
    add_library(vulkan_headers_local INTERFACE)
    target_include_directories(vulkan_headers_local INTERFACE "${Vulkan_INCLUDE_DIR}")
    set(VULKAN_HEADERS_TARGET vulkan_headers_local)
else()
    message(FATAL_ERROR "未找到 Vulkan headers 目标或头文件目录")
endif()

find_package(glfw3 CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(unofficial-shaderc CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(vma_impl OBJECT vma_impl.cpp)
target_link_libraries(vma_impl PRIVATE
    ${VULKAN_HEADERS_TARGET}
    volk::volk
    GPUOpen::VulkanMemoryAllocator
)

add_executable(VulkanWindow main.cpp $<TARGET_OBJECTS:vma_impl>)
target_link_libraries(VulkanWindow PRIVATE
    ${VULKAN_HEADERS_TARGET}
    glfw
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    unofficial::shaderc::shaderc
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

if(WIN32 AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    add_custom_command(TARGET VulkanWindow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/vulkan-1.dll,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/vulkan-1.dll>"
            "$<TARGET_FILE_DIR:VulkanWindow>"
        VERBATIM
    )
endif()
