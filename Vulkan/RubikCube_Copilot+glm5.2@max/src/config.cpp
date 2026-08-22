#include "config.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <set>

namespace rubik {

using json = nlohmann::json;

static constexpr uint32_t SUPPORTED_SCHEMA = 1;
static constexpr uint32_t MIN_TEXTURE_SIZE = 1;
static constexpr uint32_t MAX_TEXTURE_SIZE = 16384; // 合理上限

int Config::indexOf(const std::string& id) const {
    for (size_t i = 0; i < materials.size(); ++i) {
        if (materials[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

// 从 JSON 取一个长度为 3 的浮点数组，类型与长度严格校验
static void readColor3(const json& j, const char* key, float out[3]) {
    if (!j.contains(key)) throw ConfigError(std::string("材质缺少字段: ") + key);
    const json& v = j[key];
    if (!v.is_array() || v.size() != 3)
        throw ConfigError(std::string("字段 ") + key + " 必须是长度为 3 的数组");
    for (int i = 0; i < 3; ++i) {
        if (!v[i].is_number())
            throw ConfigError(std::string("字段 ") + key + " 元素必须是数字");
        float f = v[i].get<float>();
        if (key == std::string("baseColorSRGB")) {
            if (f < 0.0f || f > 1.0f)
                throw ConfigError(std::string("字段 ") + key + " 元素必须在 [0,1]");
        }
        out[i] = f;
    }
}

static float readFloat(const json& j, const char* key, float lo, float hi) {
    if (!j.contains(key)) throw ConfigError(std::string("材质缺少字段: ") + key);
    const json& v = j[key];
    if (!v.is_number()) throw ConfigError(std::string("字段 ") + key + " 必须是数字");
    float f = v.get<float>();
    if (f < lo || f > hi) {
        std::string msg = std::string("字段 ") + key + " 超出范围 [" +
            std::to_string(lo) + "," + std::to_string(hi) + "]";
        throw ConfigError(msg);
    }
    return f;
}

static uint32_t readUint(const json& j, const char* key) {
    if (!j.contains(key)) throw ConfigError(std::string("材质缺少字段: ") + key);
    const json& v = j[key];
    // nlohmann 把整数存为 number_integer/unsigned，浮点也算 number；这里要求整数语义
    if (!v.is_number())
        throw ConfigError(std::string("字段 ") + key + " 必须是整数");
    double d = v.get<double>();
    if (d < 0.0 || d != static_cast<double>(static_cast<uint32_t>(d)))
        throw ConfigError(std::string("字段 ") + key + " 必须是非负整数");
    return static_cast<uint32_t>(d);
}

static std::string readString(const json& j, const char* key) {
    if (!j.contains(key)) throw ConfigError(std::string("材质缺少字段: ") + key);
    const json& v = j[key];
    if (!v.is_string()) throw ConfigError(std::string("字段 ") + key + " 必须是字符串");
    return v.get<std::string>();
}

static Pattern readPattern(const json& j) {
    std::string s = readString(j, "pattern");
    if (s == "brushed_x") return Pattern::BrushedX;
    if (s == "molded") return Pattern::Molded;
    throw ConfigError("未知 pattern: " + s + "（仅支持 brushed_x / molded）");
}

Config loadConfig(const std::string& path) {
    Config cfg;

    std::ifstream in(path, std::ios::binary);
    if (!in) throw ConfigError("无法打开配置文件: " + path);

    json root;
    try {
        in >> root;
    } catch (const json::parse_error& e) {
        throw ConfigError(std::string("JSON 解析失败: ") + e.what());
    }
    if (!root.is_object()) throw ConfigError("配置根对象必须是 JSON 对象");

    // schemaVersion
    if (!root.contains("schemaVersion") || !root["schemaVersion"].is_number())
        throw ConfigError("缺少或非数字的 schemaVersion");
    cfg.schemaVersion = root["schemaVersion"].get<uint32_t>();
    if (cfg.schemaVersion != SUPPORTED_SCHEMA)
        throw ConfigError("不支持的 schemaVersion: " +
            std::to_string(cfg.schemaVersion) + "，仅支持 " +
            std::to_string(SUPPORTED_SCHEMA));

    // textureSize
    if (!root.contains("textureSize") || !root["textureSize"].is_number())
        throw ConfigError("缺少或非数字的 textureSize");
    int ts = root["textureSize"].get<int>();
    if (ts < static_cast<int>(MIN_TEXTURE_SIZE) ||
        ts > static_cast<int>(MAX_TEXTURE_SIZE))
        throw ConfigError("textureSize 无效: " + std::to_string(ts));
    cfg.textureSize = static_cast<uint32_t>(ts);

    // materials
    if (!root.contains("materials") || !root["materials"].is_array())
        throw ConfigError("缺少 materials 数组");
    const json& mats = root["materials"];
    if (mats.empty()) throw ConfigError("materials 数组为空");

    std::set<std::string> ids;
    for (size_t i = 0; i < mats.size(); ++i) {
        const json& m = mats[i];
        if (!m.is_object())
            throw ConfigError("materials[" + std::to_string(i) + "] 不是对象");

        Material mat;
        mat.id = readString(m, "id");
        if (mat.id.empty()) throw ConfigError("材质 id 不能为空");
        if (ids.count(mat.id))
            throw ConfigError("材质 id 重复: " + mat.id);
        ids.insert(mat.id);

        readColor3(m, "baseColorSRGB", mat.baseColorSRGB);
        mat.metallic = readFloat(m, "metallic", 0.0f, 1.0f);
        mat.roughness = readFloat(m, "roughness", 0.0f, 1.0f);
        mat.ambientOcclusion = readFloat(m, "ambientOcclusion", 0.0f, 1.0f);
        mat.normalStrength = readFloat(m, "normalStrength", 0.0f, 8.0f);
        mat.baseColorVariation = readFloat(m, "baseColorVariation", 0.0f, 1.0f);
        mat.roughnessVariation = readFloat(m, "roughnessVariation", 0.0f, 1.0f);
        mat.textureSeed = readUint(m, "textureSeed");
        mat.pattern = readPattern(m);

        cfg.materials.push_back(std::move(mat));
    }

    return cfg;
}

} // namespace rubik
