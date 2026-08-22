#pragma once
// CPU 端程序化纹理生成。
// 对每个材质生成三张：基础色(sRGB)、粗糙度(UNORM R8)、法线(UNORM RGBA8)。
// 尺寸取自 Config::textureSize；用 textureSeed 做确定性随机，相同配置可复现。

#include "config.h"
#include <vector>
#include <cstdint>

namespace rubik {

struct PixelTextures {
    uint32_t size = 0;
    // 基础色：sRGB 编码的 RGBA8（alpha=255）。上传时用 R8G8B8A8_SRGB。
    std::vector<uint8_t> baseColor; // size*size*4
    // 粗糙度：UNORM R8。
    std::vector<uint8_t> roughness; // size*size*1
    // 法线：UNORM RGBA8，切线空间，alpha=255。上传时用 R8G8B8A8_UNORM。
    std::vector<uint8_t> normal;    // size*size*4
};

// 生成单个材质的三张纹理。
PixelTextures generateTextures(const Material& mat, uint32_t textureSize);

} // namespace rubik
