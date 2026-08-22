#include "texture_gen.h"

#include <cmath>
#include <algorithm>
#include <random>

namespace rubik {

// ---- 确定性随机数 -------------------------------------------------------
// 用材质 textureSeed 作为种子，保证相同配置可复现。
// 注意：不得在代码中硬编码材质的种子或数值；本函数只接收 mat 参数。
struct Rng {
    std::mt19937 eng;
    std::uniform_real_distribution<float> u01{0.0f, 1.0f};
    std::uniform_real_distribution<float> u11{-1.0f, 1.0f};
    explicit Rng(uint32_t seed) : eng(seed) {}
    float next01() { return u01(eng); }
    float next11() { return u11(eng); }
};

// ---- Value 噪声（2D） ---------------------------------------------------
// 用哈希梯度 + 双线性插值，确定性可复现。
static float hashGrad(int ix, int iz, int seed) {
    uint32_t h = static_cast<uint32_t>(ix) * 374761393u +
                 static_cast<uint32_t>(iz) * 668265263u +
                 static_cast<uint32_t>(seed) * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float>(h) / static_cast<float>(0xffffffffu);
}

static float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }

static float valueNoise2D(float x, float z, int seed) {
    int x0 = static_cast<int>(std::floor(x));
    int z0 = static_cast<int>(std::floor(z));
    float xf = x - static_cast<float>(x0);
    float zf = z - static_cast<float>(z0);
    float v00 = hashGrad(x0,     z0,     seed);
    float v10 = hashGrad(x0 + 1, z0,     seed);
    float v01 = hashGrad(x0,     z0 + 1, seed);
    float v11 = hashGrad(x0 + 1, z0 + 1, seed);
    float u = smoothstep(xf);
    float v = smoothstep(zf);
    return (v00 * (1 - u) + v10 * u) * (1 - v) + (v01 * (1 - u) + v11 * u) * v;
}

// 分形 Value 噪声（叠加多个频率），返回 [-1,1]
static float fbm(float x, float z, int seed, int octaves, float lacunarity = 2.0f,
                 float gain = 0.5f) {
    float amp = 1.0f;
    float freq = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * (valueNoise2D(x * freq, z * freq, seed + o * 101) * 2.0f - 1.0f);
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return sum / std::max(norm, 1e-6f);
}

// ---- 8-bit 量化 --------------------------------------------------------
// 基础色数据已是 sRGB 值（来自 baseColorSRGB），直接量化存入 R8G8B8A8_SRGB 纹理，
// 由 SRGB 采样器在 shader 端自动转线性，无需在 CPU 端做 gamma 编码。
static uint8_t clamp8(float v) {
    return static_cast<uint8_t>(std::round(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// ---- 高度图 -> 法线贴图（Sobel） -----------------------------------------
// height: [size*size]，值域 [0,1]
static void heightToNormal(const std::vector<float>& height, uint32_t size,
                           float strength, std::vector<uint8_t>& outNormal) {
    outNormal.assign(static_cast<size_t>(size) * size * 4, 0);
    auto H = [&](int x, int z) -> float {
        x = std::clamp(x, 0, static_cast<int>(size) - 1);
        z = std::clamp(z, 0, static_cast<int>(size) - 1);
        return height[static_cast<size_t>(z) * size + x];
    };
    for (uint32_t z = 0; z < size; ++z) {
        for (uint32_t x = 0; x < size; ++x) {
            // Sobel
            float hl = H(static_cast<int>(x) - 1, static_cast<int>(z));
            float hr = H(static_cast<int>(x) + 1, static_cast<int>(z));
            float hd = H(static_cast<int>(x), static_cast<int>(z) - 1);
            float hu = H(static_cast<int>(x), static_cast<int>(z) + 1);
            float dx = (hr - hl) * 0.5f * strength;
            float dz = (hu - hd) * 0.5f * strength;
            // 切线空间法线
            float nx = -dx;
            float ny = -dz;
            float nz = 1.0f;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len; ny /= len; nz /= len;
            size_t idx = (static_cast<size_t>(z) * size + x) * 4;
            outNormal[idx + 0] = clamp8(nx * 0.5f + 0.5f);
            outNormal[idx + 1] = clamp8(ny * 0.5f + 0.5f);
            outNormal[idx + 2] = clamp8(nz * 0.5f + 0.5f);
            outNormal[idx + 3] = 255;
        }
    }
}

// ---- 主生成函数 ---------------------------------------------------------
PixelTextures generateTextures(const Material& mat, uint32_t textureSize) {
    PixelTextures t;
    t.size = textureSize;
    const size_t N = static_cast<size_t>(textureSize) * textureSize;
    t.baseColor.assign(N * 4, 255);
    t.roughness.assign(N, 0);
    t.normal.assign(N * 4, 255);

    Rng rng(mat.textureSeed);

    // 高度图：用于法线贴图推导
    std::vector<float> height(N, 0.5f);

    // 基础色基准（sRGB 输入值，直接用于 sRGB 纹理编码）
    float cr = mat.baseColorSRGB[0];
    float cg = mat.baseColorSRGB[1];
    float cb = mat.baseColorSRGB[2];

    const float bv = mat.baseColorVariation;
    const float rv = mat.roughnessVariation;
    const float baseR = mat.roughness;

    if (mat.pattern == Pattern::BrushedX) {
        // 沿 X 轴高频拉丝条纹 + 低频噪声
        for (uint32_t z = 0; z < textureSize; ++z) {
            for (uint32_t x = 0; x < textureSize; ++x) {
                float fu = static_cast<float>(x) / static_cast<float>(textureSize);
                float fv = static_cast<float>(z) / static_cast<float>(textureSize);
                // 高频拉丝：沿 X 的细条纹（Z 方向高频）
                float stripe = std::sin(fv * textureSize * 0.9f +
                                        rng.next01() * 0.0f) * 0.5f + 0.5f;
                // 加细颗粒抖动
                float jitter = rng.next11() * 0.15f;
                float brush = std::clamp(stripe + jitter, 0.0f, 1.0f);
                // 低频噪声（大尺度明暗变化）
                float lowFreq = fbm(fu * 4.0f, fv * 4.0f,
                                    static_cast<int>(mat.textureSeed), 4) * 0.5f + 0.5f;

                // 基础色：以 baseColorSRGB 为基准，按 bv 叠加波动
                float vary = (rng.next11() * 0.5f + (brush - 0.5f) * 0.5f +
                              (lowFreq - 0.5f) * 0.3f) * bv;
                size_t idx = (static_cast<size_t>(z) * textureSize + x) * 4;
                t.baseColor[idx + 0] = clamp8(cr + vary);
                t.baseColor[idx + 1] = clamp8(cg + vary);
                t.baseColor[idx + 2] = clamp8(cb + vary);
                t.baseColor[idx + 3] = 255;

                // 粗糙度：拉丝方向更光滑（沿 X），垂直方向更粗糙
                float dirFactor = 0.7f + 0.3f * brush; // 简化
                float r = baseR + (rng.next11() * rv) + (lowFreq - 0.5f) * rv * 0.5f;
                r *= dirFactor;
                t.roughness[static_cast<size_t>(z) * textureSize + x] = clamp8(r);

                // 高度图：拉丝凹凸
                float h = 0.5f + (brush - 0.5f) * 0.2f + (lowFreq - 0.5f) * 0.15f +
                          rng.next11() * 0.05f;
                height[static_cast<size_t>(z) * textureSize + x] = std::clamp(h, 0.0f, 1.0f);
            }
        }
    } else { // Pattern::Molded：均匀微颗粒凹凸
        for (uint32_t z = 0; z < textureSize; ++z) {
            for (uint32_t x = 0; x < textureSize; ++x) {
                float fu = static_cast<float>(x) / static_cast<float>(textureSize);
                float fv = static_cast<float>(z) / static_cast<float>(textureSize);
                // 细颗粒
                float grain = fbm(fu * 32.0f, fv * 32.0f,
                                  static_cast<int>(mat.textureSeed), 3) * 0.5f + 0.5f;
                // 大尺度色斑
                float blotch = fbm(fu * 3.0f, fv * 3.0f,
                                   static_cast<int>(mat.textureSeed) + 7, 4) * 0.5f + 0.5f;

                float vary = (rng.next11() * 0.6f + (grain - 0.5f) * 0.4f +
                              (blotch - 0.5f) * 0.5f) * bv;
                size_t idx = (static_cast<size_t>(z) * textureSize + x) * 4;
                t.baseColor[idx + 0] = clamp8(cr + vary);
                t.baseColor[idx + 1] = clamp8(cg + vary);
                t.baseColor[idx + 2] = clamp8(cb + vary);
                t.baseColor[idx + 3] = 255;

                float r = baseR + (rng.next11() * rv) + (grain - 0.5f) * rv * 0.4f;
                t.roughness[static_cast<size_t>(z) * textureSize + x] = clamp8(r);

                float h = 0.5f + (grain - 0.5f) * 0.25f + rng.next11() * 0.04f;
                height[static_cast<size_t>(z) * textureSize + x] = std::clamp(h, 0.0f, 1.0f);
            }
        }
    }

    // 法线贴图由高度图推导，整体强度按 normalStrength 缩放
    heightToNormal(height, textureSize, mat.normalStrength, t.normal);

    return t;
}

} // namespace rubik
