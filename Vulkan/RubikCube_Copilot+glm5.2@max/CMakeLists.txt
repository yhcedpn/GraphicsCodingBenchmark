cmake_minimum_required(VERSION 3.24)

# 使用当前项目已有的构建入口和配置；依赖声明见 vcpkg.json，
# 实际查找按分层回退：官方 cmake config -> CMake FindModule -> pkg-config。
# 不假定特定操作系统/包管理器。

project(RubikCube LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 输出目录统一，便于运行时找到 materials.json
set(RUBIK_RUNTIME_OUTPUT ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${RUBIK_RUNTIME_OUTPUT})

# ---- 依赖查找（分层回退，不假定 OS/包管理器） ---------------------------

# Vulkan headers + loader
find_package(Vulkan REQUIRED)
# Vulkan 1.4 headers（vulkan-headers 1.4.357 提供 VK_API_VERSION_1_4）
if(Vulkan_VERSION VERSION_LESS 1.4)
    message(FATAL_ERROR "需要 Vulkan headers >= 1.4，当前 ${Vulkan_VERSION}")
endif()

# volk：函数加载（不链接 libvulkan 与 volk 符号冲突由 config 处理）
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
find_package(volk REQUIRED CONFIG)

# GLFW：窗口与 surface
find_package(glfw3 CONFIG QUIET)
if(NOT glfw3_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(GLFW IMPORTED_TARGET glfw3)
    endif()
endif()
if(NOT TARGET glfw AND TARGET glfw3::glfw3)
    add_library(glfw ALIAS glfw3::glfw3)
endif()

# VMA（header-only + 官方 cmake config）
find_package(VulkanMemoryAllocator REQUIRED CONFIG)

# shaderc：运行时 GLSL -> SPIR-V 编译
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SHADERC IMPORTED_TARGET shaderc)
endif()
if(NOT SHADERC_FOUND)
    # 回退：手动查找
    find_path(SHADERC_INCLUDE_DIR shaderc/shaderc.h)
    find_library(SHADERC_LIBRARY NAMES shaderc_shared)
    if(SHADERC_INCLUDE_DIR AND SHADERC_LIBRARY)
        add_library(shaderc::shaderc INTERFACE IMPORTED)
        set_target_properties(shaderc::shaderc PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${SHADERC_INCLUDE_DIR}
            INTERFACE_LINK_LIBRARIES ${SHADERC_LIBRARY})
    endif()
endif()

# nlohmann-json：配置读取
find_package(nlohmann_json REQUIRED CONFIG)

# ---- 目标 ---------------------------------------------------------------

add_executable(rubik_cube
    src/main.cpp
    src/config.cpp
    src/texture_gen.cpp
    src/scene.cpp
    src/vulkan_engine.cpp
)

target_include_directories(rubik_cube PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(rubik_cube PRIVATE
    Vulkan::Headers
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    nlohmann_json::nlohmann_json
)

# GLFW
if(TARGET glfw)
    target_link_libraries(rubik_cube PRIVATE glfw)
elseif(TARGET PkgConfig::GLFW)
    target_link_libraries(rubik_cube PRIVATE PkgConfig::GLFW)
else()
    message(FATAL_ERROR "未找到 glfw3")
endif()

# shaderc
if(TARGET shaderc::shaderc)
    target_link_libraries(rubik_cube PRIVATE shaderc::shaderc)
elseif(TARGET PkgConfig::SHADERC)
    target_link_libraries(rubik_cube PRIVATE PkgConfig::SHADERC)
else()
    message(FATAL_ERROR "未找到 shaderc")
endif()

# volk 通过 dlopen 加载 libvulkan，不链接 libvulkan；链接 dl
target_link_libraries(rubik_cube PRIVATE ${CMAKE_DL_LIBS})

target_compile_options(rubik_cube PRIVATE -Wall -Wextra -Wpedantic)

# 运行时需要 materials.json 与二进制同目录
add_custom_command(TARGET rubik_cube POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/materials.json
        $<TARGET_FILE_DIR:rubik_cube>/materials.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/platforms.json
        $<TARGET_FILE_DIR:rubik_cube>/platforms.json
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json
        $<TARGET_FILE_DIR:rubik_cube>/vcpkg.json
    # 着色器目录也拷贝一份，便于从 build 目录直接运行（代码内亦回退到 ../shaders）
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/shaders
        $<TARGET_FILE_DIR:rubik_cube>/shaders
    VERBATIM)
