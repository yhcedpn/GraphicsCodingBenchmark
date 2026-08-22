// Vulkan 1.4 PBR RubikCube 场景渲染入口。
// 运行时读取当前目录 materials.json；材质数值唯一来源为该文件。

#include "config.h"
#include "vulkan_engine.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main() {
    rubik::Config cfg;
    try {
        cfg = rubik::loadConfig("materials.json");
    } catch (const rubik::ConfigError& e) {
        std::fprintf(stderr, "配置错误: %s\n", e.what());
        return EXIT_FAILURE;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "配置读取异常: %s\n", e.what());
        return EXIT_FAILURE;
    }

    std::printf("已加载 %zu 个材质，textureSize=%u\n",
                cfg.materials.size(), cfg.textureSize);

    rubik::VulkanEngine engine;
    try {
        engine.init(cfg);
        engine.run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "运行错误: %s\n", e.what());
        engine.cleanup();
        return EXIT_FAILURE;
    }
    engine.cleanup();
    glfwTerminate();
    return EXIT_SUCCESS;
}
