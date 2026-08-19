cmake_minimum_required(VERSION 3.28)

project(VoxelPBRFrustumCulling LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # 源码含简体中文 UTF-8 字符串字面量，统一按 UTF-8 解释。
    add_compile_options(/utf-8)
endif()

find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(VoxelPBRFrustumCulling main.cpp)
target_link_libraries(VoxelPBRFrustumCulling PRIVATE glfw glad::glad glm::glm nlohmann_json::nlohmann_json)
