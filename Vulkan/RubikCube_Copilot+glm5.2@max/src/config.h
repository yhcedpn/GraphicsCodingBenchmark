#pragma once
// materials.json 配置读取与校验。
// 所有材质数值的唯一来源；程序不得在别处硬编码这些数值。

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace rubik {

enum class Pattern : int {
    BrushedX, // 沿 X 轴拉丝条纹（金属）
    Molded,   // 均匀微颗粒凹凸（注塑塑料）
};

struct Material {
    std::string id;
    float baseColorSRGB[3] = {0, 0, 0};
    float metallic = 0.0f;
    float roughness = 0.0f;
    float ambientOcclusion = 0.0f;
    float normalStrength = 0.0f;
    float baseColorVariation = 0.0f;
    float roughnessVariation = 0.0f;
    uint32_t textureSeed = 0;
    Pattern pattern = Pattern::Molded;
};

struct Config {
    uint32_t schemaVersion = 0;
    uint32_t textureSize = 0; // 宽=高=textureSize
    std::vector<Material> materials;
    // id -> 索引，运行时查表
    int indexOf(const std::string& id) const;
};

// 读取并校验当前目录的 materials.json；任何错误抛 ConfigError。
// 严格校验：文件缺失/无法解析/schemaVersion 不支持/textureSize 无效/
// 材质数组为空/ID 重复/缺必需字段/字段类型或数组长度错误。
class ConfigError : public std::runtime_error {
public:
    explicit ConfigError(const std::string& msg) : std::runtime_error(msg) {}
};

// 从指定路径加载（默认 "materials.json"）。
Config loadConfig(const std::string& path = "materials.json");

} // namespace rubik
