cmake_minimum_required(VERSION 3.28)

project(ProceduralDeferredRenderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

add_executable(ProceduralDeferredRenderer main.cpp)
target_link_libraries(ProceduralDeferredRenderer PRIVATE glfw glad::glad glm::glm)
