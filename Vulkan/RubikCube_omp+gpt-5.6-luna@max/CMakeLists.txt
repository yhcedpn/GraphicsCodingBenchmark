cmake_minimum_required(VERSION 3.28)

project(RubikCube LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(RubikCube main.cpp shaders.inc)
set_source_files_properties(shaders.inc PROPERTIES HEADER_FILE_ONLY ON)

target_link_libraries(RubikCube PRIVATE
    glfw
    volk::volk
    Vulkan::Vulkan
    nlohmann_json::nlohmann_json
)

if(MSVC)
    target_compile_options(RubikCube PRIVATE /utf-8)
endif()

add_custom_command(TARGET RubikCube POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/materials.json"
        "$<TARGET_FILE_DIR:RubikCube>/materials.json"
    VERBATIM
)

include(GNUInstallDirs)
install(TARGETS RubikCube RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES materials.json DESTINATION ${CMAKE_INSTALL_BINDIR})
