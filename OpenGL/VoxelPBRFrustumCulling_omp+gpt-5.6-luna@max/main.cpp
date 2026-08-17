#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define GLFW_INCLUDE_NONE
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kRenderWidth = 640;
constexpr int kRenderHeight = 360;
constexpr int kShadowResolution = 2048;
constexpr int kEnvironmentSize = 256;
constexpr int kIrradianceSize = 32;
constexpr int kPrefilterSize = 128;
constexpr int kPrefilterLevels = 5;
constexpr int kBrdfSize = 256;
constexpr int kExpectedInstanceCount = 250;
constexpr float kPi = 3.14159265358979323846f;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string pathText(const fs::path& path) {
    return path.string();
}

fs::path executableDirectory() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return fs::path(std::wstring(buffer.data(), length)).parent_path();
}

fs::path findProjectRoot() {
    std::vector<fs::path> starts;
    std::error_code error;
    starts.push_back(fs::current_path(error));
    const fs::path executablePath = executableDirectory();
    if (!executablePath.empty()) {
        starts.push_back(executablePath);
    }

    std::unordered_set<std::string> visited;
    for (const fs::path& start : starts) {
        if (start.empty()) {
            continue;
        }
        fs::path candidate = fs::weakly_canonical(start, error);
        if (error) {
            error.clear();
            candidate = start;
        }
        for (int depth = 0; depth < 8 && !candidate.empty(); ++depth) {
            const std::string key = pathText(candidate);
            if (!visited.insert(key).second) {
                break;
            }
            if (fs::is_regular_file(candidate / "materials.json", error)) {
                return candidate;
            }
            error.clear();
            const fs::path parent = candidate.parent_path();
            if (parent == candidate) {
                break;
            }
            candidate = parent;
        }
    }
    fail("无法定位 materials.json；请从工程目录或 bin/x64/Debug 启动程序");
}

std::string readTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail("无法读取文本资源: " + pathText(path));
    }
    std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF) {
        source.erase(0, 3);
    }
    return source;
}

struct MaterialData {
    std::string id;
    glm::vec3 baseColorSRGB{};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ambientOcclusion = 1.0f;
    float normalStrength = 0.0f;
    float baseColorVariation = 0.0f;
    float roughnessVariation = 0.0f;
    std::uint32_t textureSeed = 0;
    std::string pattern;
};

struct LoadedMaterials {
    int textureSize = 0;
    std::vector<MaterialData> materials;
    int groundLayer = -1;
    int pillarLayer = -1;
};

const json& requiredField(const json& object, const char* field, const std::string& path) {
    if (!object.is_object() || !object.contains(field)) {
        fail(path + "." + field + " 缺失");
    }
    return object.at(field);
}

float requiredNumber(const json& value, const std::string& path) {
    if (!value.is_number()) {
        fail(path + " 必须是 number");
    }
    return value.get<float>();
}

int requiredInteger(const json& value, const std::string& path) {
    if (!value.is_number_integer()) {
        fail(path + " 必须是 integer");
    }
    return value.get<int>();
}

std::string requiredString(const json& value, const std::string& path) {
    if (!value.is_string()) {
        fail(path + " 必须是 string");
    }
    return value.get<std::string>();
}

glm::vec3 requiredVec3(const json& value, const std::string& path) {
    if (!value.is_array() || value.size() != 3) {
        fail(path + " 必须是长度为 3 的数组");
    }
    return glm::vec3(
        requiredNumber(value.at(0), path + "[0]"),
        requiredNumber(value.at(1), path + "[1]"),
        requiredNumber(value.at(2), path + "[2]")
    );
}

LoadedMaterials loadMaterials(const fs::path& root) {
    const fs::path path = root / "materials.json";
    const std::string source = readTextFile(path);
    json document;
    try {
        document = json::parse(source);
    } catch (const json::parse_error& error) {
        fail(pathText(path) + " JSON 语法错误: " + error.what());
    }
    if (!document.is_object()) {
        fail(pathText(path) + " 根节点必须是 object");
    }

    const int schemaVersion = requiredInteger(requiredField(document, "schemaVersion", "$"), "$.schemaVersion");
    if (schemaVersion != 1) {
        fail("$.schemaVersion 不支持: " + std::to_string(schemaVersion) + "，仅支持 1");
    }
    const int textureSize = requiredInteger(requiredField(document, "textureSize", "$"), "$.textureSize");
    if (textureSize != 32) {
        fail("$.textureSize 必须为 32，实际为 " + std::to_string(textureSize));
    }
    const json& materialArray = requiredField(document, "materials", "$");
    if (!materialArray.is_array() || materialArray.size() != 2) {
        fail("$.materials 必须是包含两种材质的数组");
    }

    LoadedMaterials result;
    result.textureSize = textureSize;
    result.materials.reserve(materialArray.size());
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < materialArray.size(); ++index) {
        const json& object = materialArray.at(index);
        const std::string prefix = "$.materials[" + std::to_string(index) + "]";
        if (!object.is_object()) {
            fail(prefix + " 必须是 object");
        }
        MaterialData material;
        material.id = requiredString(requiredField(object, "id", prefix), prefix + ".id");
        if (material.id != "brushed_metal" && material.id != "red_plastic") {
            fail(prefix + ".id 不支持: " + material.id);
        }
        if (!ids.insert(material.id).second) {
            fail(prefix + ".id 重复: " + material.id);
        }
        material.baseColorSRGB = requiredVec3(requiredField(object, "baseColorSRGB", prefix), prefix + ".baseColorSRGB");
        material.metallic = requiredNumber(requiredField(object, "metallic", prefix), prefix + ".metallic");
        material.roughness = requiredNumber(requiredField(object, "roughness", prefix), prefix + ".roughness");
        material.ambientOcclusion = requiredNumber(requiredField(object, "ambientOcclusion", prefix), prefix + ".ambientOcclusion");
        material.normalStrength = requiredNumber(requiredField(object, "normalStrength", prefix), prefix + ".normalStrength");
        material.baseColorVariation = requiredNumber(requiredField(object, "baseColorVariation", prefix), prefix + ".baseColorVariation");
        material.roughnessVariation = requiredNumber(requiredField(object, "roughnessVariation", prefix), prefix + ".roughnessVariation");
        const int seed = requiredInteger(requiredField(object, "textureSeed", prefix), prefix + ".textureSeed");
        if (seed < 0) {
            fail(prefix + ".textureSeed 必须非负");
        }
        material.textureSeed = static_cast<std::uint32_t>(seed);
        material.pattern = requiredString(requiredField(object, "pattern", prefix), prefix + ".pattern");

        for (int channel = 0; channel < 3; ++channel) {
            if (material.baseColorSRGB[channel] < 0.0f || material.baseColorSRGB[channel] > 1.0f) {
                fail(prefix + ".baseColorSRGB 必须位于 [0,1]");
            }
        }
        if (material.metallic < 0.0f || material.metallic > 1.0f) {
            fail(prefix + ".metallic 必须位于 [0,1]");
        }
        if (material.roughness < 0.0f || material.roughness > 1.0f) {
            fail(prefix + ".roughness 必须位于 [0,1]");
        }
        if (material.ambientOcclusion < 0.0f || material.ambientOcclusion > 1.0f) {
            fail(prefix + ".ambientOcclusion 必须位于 [0,1]");
        }
        if (material.normalStrength < 0.0f || material.normalStrength > 1.0f) {
            fail(prefix + ".normalStrength 必须位于 [0,1]");
        }
        if (material.baseColorVariation < 0.0f || material.baseColorVariation > 1.0f) {
            fail(prefix + ".baseColorVariation 必须位于 [0,1]");
        }
        if (material.roughnessVariation < 0.0f || material.roughnessVariation > 1.0f ||
            material.roughness - material.roughnessVariation < 0.0f ||
            material.roughness + material.roughnessVariation > 1.0f) {
            fail(prefix + ".roughnessVariation 使 roughness 超出 [0,1]");
        }
        if ((material.id == "brushed_metal" && material.pattern != "brushed_x") ||
            (material.id == "red_plastic" && material.pattern != "molded")) {
            fail(prefix + ".pattern 与材质 id 不匹配");
        }
        result.materials.push_back(std::move(material));
    }

    for (std::size_t index = 0; index < result.materials.size(); ++index) {
        if (result.materials[index].id == "brushed_metal") {
            result.groundLayer = static_cast<int>(index);
        } else if (result.materials[index].id == "red_plastic") {
            result.pillarLayer = static_cast<int>(index);
        }
    }
    if (result.groundLayer < 0 || result.pillarLayer < 0) {
        fail("$.materials 必须同时包含 brushed_metal 与 red_plastic");
    }
    return result;
}

std::uint32_t hashCoordinates(std::uint32_t seed, std::uint32_t x, std::uint32_t y) {
    std::uint32_t value = seed + 0x9e3779b9u;
    value ^= x + 0x85ebca6bu + (value << 6u) + (value >> 2u);
    value ^= y + 0xc2b2ae35u + (value << 6u) + (value >> 2u);
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float noise01(std::uint32_t seed, std::uint32_t x, std::uint32_t y) {
    return static_cast<float>(hashCoordinates(seed, x, y) & 0x00ffffffu) / 16777215.0f;
}

float signedNoise(std::uint32_t seed, std::uint32_t x, std::uint32_t y) {
    return noise01(seed, x, y) * 2.0f - 1.0f;
}

unsigned char toByte(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<unsigned char>(std::lround(clamped * 255.0f));
}
unsigned char toByteInRange(float value, float lower, float upper) {
    const float clampedLower = std::clamp(lower, 0.0f, 1.0f);
    const float clampedUpper = std::clamp(upper, 0.0f, 1.0f);
    const int minimumByte = static_cast<int>(std::ceil(clampedLower * 255.0f));
    const int maximumByte = static_cast<int>(std::floor(clampedUpper * 255.0f));
    const int roundedByte = static_cast<int>(std::lround(std::clamp(value, clampedLower, clampedUpper) * 255.0f));
    return static_cast<unsigned char>(std::clamp(roundedByte, minimumByte, maximumByte));
}

struct GeneratedTextures {
    std::vector<std::uint8_t> baseColor;
    std::vector<std::uint8_t> normal;
    std::vector<std::uint8_t> orm;
};

GeneratedTextures generateMaterialTextures(const LoadedMaterials& materials) {
    const std::size_t layerTexelCount = static_cast<std::size_t>(materials.textureSize) * materials.textureSize;
    const std::size_t byteCount = layerTexelCount * 4 * materials.materials.size();
    GeneratedTextures generated;
    generated.baseColor.resize(byteCount);
    generated.normal.resize(byteCount);
    generated.orm.resize(byteCount);

    for (std::size_t layer = 0; layer < materials.materials.size(); ++layer) {
        const MaterialData& material = materials.materials[layer];
        for (int y = 0; y < materials.textureSize; ++y) {
            for (int x = 0; x < materials.textureSize; ++x) {
                const std::size_t texel = (layer * layerTexelCount + static_cast<std::size_t>(y) * materials.textureSize + x) * 4;
                const std::uint32_t ux = static_cast<std::uint32_t>(x);
                const std::uint32_t uy = static_cast<std::uint32_t>(y);
                float colorVariation = signedNoise(material.textureSeed, ux, uy);
                float roughnessNoise = signedNoise(material.textureSeed ^ 0x51ed270bu, ux, uy);
                float normalX = signedNoise(material.textureSeed ^ 0x12a4f39du, ux, uy);
                float normalY = signedNoise(material.textureSeed ^ 0x9e3779b9u, ux, uy);
                if (material.pattern == "brushed_x") {
                    const float brushLine = signedNoise(material.textureSeed, uy, 0u);
                    const float brushMicro = signedNoise(material.textureSeed ^ 0xa5a5a5a5u, ux, uy);
                    colorVariation = brushLine * 0.78f + brushMicro * 0.22f;
                    roughnessNoise = signedNoise(material.textureSeed ^ 0x51ed270bu, uy, 0u) * 0.8f +
                        signedNoise(material.textureSeed ^ 0x6d2b79f5u, ux, uy) * 0.2f;
                    normalX *= 0.18f;
                    normalY = brushLine * 0.82f + normalY * 0.18f;
                }

                for (int channel = 0; channel < 3; ++channel) {
                    generated.baseColor[texel + channel] = toByteInRange(
                        material.baseColorSRGB[channel] + colorVariation * material.baseColorVariation,
                        material.baseColorSRGB[channel] - material.baseColorVariation,
                        material.baseColorSRGB[channel] + material.baseColorVariation
                    );
                }
                generated.baseColor[texel + 3] = 255;

                const float tangentX = normalX * material.normalStrength;
                const float tangentY = normalY * material.normalStrength;
                generated.normal[texel + 0] = toByteInRange(
                    0.5f + tangentX * 0.5f,
                    0.5f - material.normalStrength * 0.5f,
                    0.5f + material.normalStrength * 0.5f
                );
                generated.normal[texel + 1] = toByteInRange(
                    0.5f + tangentY * 0.5f,
                    0.5f - material.normalStrength * 0.5f,
                    0.5f + material.normalStrength * 0.5f
                );
                generated.normal[texel + 2] = 255;
                generated.normal[texel + 3] = 255;

                generated.orm[texel + 0] = toByte(material.ambientOcclusion);
                generated.orm[texel + 1] = toByteInRange(
                    material.roughness + roughnessNoise * material.roughnessVariation,
                    material.roughness - material.roughnessVariation,
                    material.roughness + material.roughnessVariation
                );
                generated.orm[texel + 2] = toByte(material.metallic);
                generated.orm[texel + 3] = 255;
            }
        }
    }
    return generated;
}

GLuint compileShader(GLenum type, const fs::path& path) {
    const std::string source = readTextFile(path);
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        fail("创建 shader 失败: " + pathText(path));
    }
    const char* sourcePointer = source.c_str();
    glShaderSource(shader, 1, &sourcePointer, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        const std::string stage = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
        std::cerr << "Shader 编译失败 [" << stage << "] " << pathText(path) << "\n" << log.data() << std::endl;
        glDeleteShader(shader);
        fail("shader 编译失败: " + pathText(path));
    }
    return shader;
}

GLuint createProgram(const fs::path& shaderDirectory, const std::string& vertexFile, const std::string& fragmentFile) {
    const fs::path vertexPath = shaderDirectory / vertexFile;
    const fs::path fragmentPath = shaderDirectory / fragmentFile;
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexPath);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentPath);
    const GLuint program = glCreateProgram();
    if (program == 0) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        fail("创建 shader program 失败: " + vertexFile + ", " + fragmentFile);
    }
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (linked != GL_TRUE) {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        std::cerr << "Program 链接失败 [" << vertexFile << ", " << fragmentFile << "]\n" << log.data() << std::endl;
        glDeleteProgram(program);
        fail("program 链接失败: " + vertexFile + ", " + fragmentFile);
    }
    return program;
}

GLint uniformLocation(GLuint program, const char* name) {
    return glGetUniformLocation(program, name);
}

void setSampler(GLuint program, const char* name, GLint unit) {
    const GLint location = uniformLocation(program, name);
    if (location >= 0) {
        glUseProgram(program);
        glUniform1i(location, unit);
    }
}

void checkFramebuffer(const char* label) {
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::ostringstream message;
        message << label << " framebuffer 不完整，状态码 0x" << std::hex << status;
        fail(message.str());
    }
}

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    glm::vec3 tangent;
};

struct alignas(16) GpuInstance {
    glm::mat4 model{1.0f};
    std::int32_t materialIndex = 0;
    std::int32_t padding[3]{};
};

struct Instance {
    glm::mat4 model{1.0f};
    glm::vec3 boundsMin{};
    glm::vec3 boundsMax{};
    GpuInstance gpu{};
};

struct Plane {
    glm::vec3 normal{};
    float distance = 0.0f;
};

struct Frustum {
    std::array<Plane, 6> planes{};

    static Frustum fromMatrix(const glm::mat4& matrix) {
        const glm::vec4 row0(matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0]);
        const glm::vec4 row1(matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1]);
        const glm::vec4 row2(matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2]);
        const glm::vec4 row3(matrix[0][3], matrix[1][3], matrix[2][3], matrix[3][3]);
        const std::array<glm::vec4, 6> coefficients = {
            row3 + row0, row3 - row0,
            row3 + row1, row3 - row1,
            row3 + row2, row3 - row2
        };
        Frustum frustum;
        for (std::size_t index = 0; index < coefficients.size(); ++index) {
            const float length = glm::length(glm::vec3(coefficients[index]));
            if (length <= std::numeric_limits<float>::epsilon()) {
                frustum.planes[index] = Plane{};
            } else {
                frustum.planes[index].normal = glm::vec3(coefficients[index]) / length;
                frustum.planes[index].distance = coefficients[index].w / length;
            }
        }
        return frustum;
    }

    bool intersects(const Instance& instance) const {
        for (const Plane& plane : planes) {
            const glm::vec3 positive(
                plane.normal.x >= 0.0f ? instance.boundsMax.x : instance.boundsMin.x,
                plane.normal.y >= 0.0f ? instance.boundsMax.y : instance.boundsMin.y,
                plane.normal.z >= 0.0f ? instance.boundsMax.z : instance.boundsMin.z
            );
            if (glm::dot(plane.normal, positive) + plane.distance < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

struct Camera {
    glm::vec3 position{18.0f, 14.0f, 18.0f};
    float yaw = -135.0f;
    float pitch = -25.0f;

    glm::vec3 front() const {
        const float yawRadians = glm::radians(yaw);
        const float pitchRadians = glm::radians(pitch);
        return glm::normalize(glm::vec3(
            std::cos(yawRadians) * std::cos(pitchRadians),
            std::sin(pitchRadians),
            std::sin(yawRadians) * std::cos(pitchRadians)
        ));
    }

    glm::mat4 viewMatrix() const {
        return glm::lookAt(position, position + front(), glm::vec3(0.0f, 1.0f, 0.0f));
    }
};

struct AppState {
    GLFWwindow* window = nullptr;
    Camera camera{};
    bool cursorCaptured = true;
    bool cullingEnabled = true;
    int debugMode = 1;
    bool firstMouse = true;
    double lastMouseX = 0.0;
    double lastMouseY = 0.0;
    int framebufferWidth = kWindowWidth;
    int framebufferHeight = kWindowHeight;
};

void framebufferSizeCallback(GLFWwindow* window, int width, int height) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (state != nullptr) {
        state->framebufferWidth = width;
        state->framebufferHeight = height;
    }
}

void cursorPositionCallback(GLFWwindow* window, double x, double y) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (state == nullptr || !state->cursorCaptured) {
        return;
    }
    if (state->firstMouse) {
        state->lastMouseX = x;
        state->lastMouseY = y;
        state->firstMouse = false;
        return;
    }
    const double offsetX = x - state->lastMouseX;
    const double offsetY = y - state->lastMouseY;
    state->lastMouseX = x;
    state->lastMouseY = y;
    constexpr float sensitivity = 0.08f;
    state->camera.yaw += static_cast<float>(offsetX) * sensitivity;
    state->camera.pitch -= static_cast<float>(offsetY) * sensitivity;
    state->camera.pitch = std::clamp(state->camera.pitch, -89.0f, 89.0f);
}

void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (state == nullptr || action != GLFW_PRESS) {
        return;
    }
    if (key == GLFW_KEY_ESCAPE) {
        if (state->cursorCaptured) {
            state->cursorCaptured = false;
            state->firstMouse = true;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    } else if (key == GLFW_KEY_C) {
        state->cullingEnabled = !state->cullingEnabled;
    } else if (key >= GLFW_KEY_1 && key <= GLFW_KEY_5) {
        state->debugMode = key - GLFW_KEY_0;
    }
}

void mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (state == nullptr || button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS || state->cursorCaptured) {
        return;
    }
    state->cursorCaptured = true;
    state->firstMouse = true;
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void updateCamera(AppState& state, float deltaTime) {
    glm::vec3 forward = state.camera.front();
    forward.y = 0.0f;
    if (glm::dot(forward, forward) <= std::numeric_limits<float>::epsilon()) {
        forward = glm::vec3(0.0f, 0.0f, -1.0f);
    } else {
        forward = glm::normalize(forward);
    }
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    glm::vec3 movement(0.0f);
    if (glfwGetKey(state.window, GLFW_KEY_W) == GLFW_PRESS) {
        movement += forward;
    }
    if (glfwGetKey(state.window, GLFW_KEY_S) == GLFW_PRESS) {
        movement -= forward;
    }
    if (glfwGetKey(state.window, GLFW_KEY_D) == GLFW_PRESS) {
        movement += right;
    }
    if (glfwGetKey(state.window, GLFW_KEY_A) == GLFW_PRESS) {
        movement -= right;
    }
    if (glm::dot(movement, movement) > std::numeric_limits<float>::epsilon()) {
        state.camera.position += glm::normalize(movement) * (8.0f * deltaTime);
    }
}

struct alignas(16) FrameBlockData {
    glm::mat4 viewProjection{1.0f};
    glm::mat4 lightSpace{1.0f};
    glm::vec4 cameraPosition{};
    glm::vec4 sunDirection{};
    glm::vec4 sunRadiance{};
    glm::vec4 pointPositions[4]{};
    glm::vec4 pointColors[4]{};
};

class Renderer {
public:
    bool contextReady = false;
    GLuint sceneProgram = 0;
    GLuint shadowProgram = 0;
    GLuint environmentProgram = 0;
    GLuint irradianceProgram = 0;
    GLuint prefilterProgram = 0;
    GLuint brdfProgram = 0;
    GLuint presentProgram = 0;
    GLuint cubeVbo = 0;
    GLuint cubeEbo = 0;
    GLuint sceneVao = 0;
    GLuint shadowVao = 0;
    GLuint captureVao = 0;
    GLuint fullscreenVao = 0;
    GLuint sceneInstanceVbo = 0;
    GLuint shadowInstanceVbo = 0;
    GLuint frameUbo = 0;
    GLuint baseColorArray = 0;
    GLuint normalArray = 0;
    GLuint ormArray = 0;
    GLuint sceneFbo = 0;
    GLuint sceneColor = 0;
    GLuint sceneDepth = 0;
    GLuint shadowFbo = 0;
    GLuint shadowMap = 0;
    GLuint captureFbo = 0;
    GLuint captureDepth = 0;
    GLuint environmentMap = 0;
    GLuint irradianceMap = 0;
    GLuint prefilterMap = 0;
    GLuint brdfLut = 0;
    GLsizei cubeIndexCount = 0;
    std::vector<Instance> groundInstances;
    std::vector<Instance> pillarInstances;
    std::vector<GpuInstance> shadowInstances;

    void initialize(const fs::path& root, const LoadedMaterials& materials) {
        createCubeGeometry(materials);
        const fs::path shaderDirectory = root / "shaders";
        sceneProgram = createProgram(shaderDirectory, "scene.vert", "scene.frag");
        shadowProgram = createProgram(shaderDirectory, "shadow.vert", "shadow.frag");
        environmentProgram = createProgram(shaderDirectory, "ibl_capture.vert", "environment.frag");
        irradianceProgram = createProgram(shaderDirectory, "ibl_capture.vert", "irradiance.frag");
        prefilterProgram = createProgram(shaderDirectory, "ibl_capture.vert", "prefilter.frag");
        brdfProgram = createProgram(shaderDirectory, "fullscreen.vert", "brdf.frag");
        presentProgram = createProgram(shaderDirectory, "fullscreen.vert", "fullscreen.frag");
        createMaterialTextures(materials);
        createSceneTargets();
        createShadowTargets();
        createIblTargets();
        createFrameBuffer();
        configureSamplers();
        buildInstances(materials);
    }

    void destroy() {
        if (!contextReady) {
            return;
        }
        const std::array<GLuint, 7> programs = {
            sceneProgram, shadowProgram, environmentProgram, irradianceProgram,
            prefilterProgram, brdfProgram, presentProgram
        };
        for (const GLuint program : programs) {
            if (program != 0) {
                glDeleteProgram(program);
            }
        }
        const std::array<GLuint, 4> vertexArrays = {sceneVao, shadowVao, captureVao, fullscreenVao};
        glDeleteVertexArrays(static_cast<GLsizei>(vertexArrays.size()), vertexArrays.data());
        const std::array<GLuint, 5> buffers = {cubeVbo, cubeEbo, sceneInstanceVbo, shadowInstanceVbo, frameUbo};
        glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
        const std::array<GLuint, 10> textures = {
            baseColorArray, normalArray, ormArray, sceneColor, sceneDepth,
            shadowMap, environmentMap, irradianceMap, prefilterMap, brdfLut
        };
        glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
        const std::array<GLuint, 3> framebuffers = {sceneFbo, shadowFbo, captureFbo};
        glDeleteFramebuffers(static_cast<GLsizei>(framebuffers.size()), framebuffers.data());
        if (captureDepth != 0) {
            glDeleteRenderbuffers(1, &captureDepth);
        }
        sceneProgram = shadowProgram = environmentProgram = irradianceProgram = 0;
        prefilterProgram = brdfProgram = presentProgram = 0;
        cubeVbo = cubeEbo = sceneVao = shadowVao = captureVao = fullscreenVao = 0;
        sceneInstanceVbo = shadowInstanceVbo = frameUbo = 0;
        baseColorArray = normalArray = ormArray = 0;
        sceneFbo = sceneColor = sceneDepth = shadowFbo = shadowMap = 0;
        captureFbo = captureDepth = environmentMap = irradianceMap = prefilterMap = brdfLut = 0;
        contextReady = false;
    }

    void renderShadow(const glm::mat4& lightSpace) const {
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
        glViewport(0, 0, kShadowResolution, kShadowResolution);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUseProgram(shadowProgram);
        const GLint lightSpaceLocation = uniformLocation(shadowProgram, "uLightSpace");
        glUniformMatrix4fv(lightSpaceLocation, 1, GL_FALSE, glm::value_ptr(lightSpace));
        glBindVertexArray(shadowVao);
        glDrawElementsInstanced(GL_TRIANGLES, cubeIndexCount, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(shadowInstances.size()));
    }

    int renderMain(const FrameBlockData& frame, const std::vector<GpuInstance>& visibleGround,
                   const std::vector<GpuInstance>& visiblePillars, int debugMode) {
        glBindBuffer(GL_UNIFORM_BUFFER, frameUbo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(FrameBlockData), &frame);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameUbo);

        glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo);
        glViewport(0, 0, kRenderWidth, kRenderHeight);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glClearColor(0.015f, 0.02f, 0.035f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(sceneProgram);
        const GLint debugLocation = uniformLocation(sceneProgram, "uDebugMode");
        glUniform1i(debugLocation, debugMode);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, baseColorArray);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, normalArray);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, ormArray);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_CUBE_MAP, irradianceMap);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_CUBE_MAP, prefilterMap);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, brdfLut);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, shadowMap);
        glBindVertexArray(sceneVao);

        int drawCalls = 0;
        uploadVisible(visibleGround);
        if (!visibleGround.empty()) {
            glDrawElementsInstanced(GL_TRIANGLES, cubeIndexCount, GL_UNSIGNED_INT, nullptr,
                                    static_cast<GLsizei>(visibleGround.size()));
            ++drawCalls;
        }
        uploadVisible(visiblePillars);
        if (!visiblePillars.empty()) {
            glDrawElementsInstanced(GL_TRIANGLES, cubeIndexCount, GL_UNSIGNED_INT, nullptr,
                                    static_cast<GLsizei>(visiblePillars.size()));
            ++drawCalls;
        }
        return drawCalls;
    }

    void present(const AppState& state) const {
        if (state.framebufferWidth <= 0 || state.framebufferHeight <= 0) {
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        int viewportWidth = state.framebufferWidth;
        int viewportHeight = state.framebufferWidth * 9 / 16;
        if (viewportHeight > state.framebufferHeight) {
            viewportHeight = state.framebufferHeight;
            viewportWidth = state.framebufferHeight * 16 / 9;
        }
        const int viewportX = (state.framebufferWidth - viewportWidth) / 2;
        const int viewportY = (state.framebufferHeight - viewportHeight) / 2;
        glViewport(viewportX, viewportY, std::max(viewportWidth, 1), std::max(viewportHeight, 1));
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(presentProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneColor);
        glBindVertexArray(fullscreenVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    std::pair<std::vector<GpuInstance>, std::vector<GpuInstance>> visibleInstances(
        const Frustum& frustum, bool cullingEnabled) const {
        std::vector<GpuInstance> visibleGround;
        std::vector<GpuInstance> visiblePillars;
        visibleGround.reserve(groundInstances.size());
        visiblePillars.reserve(pillarInstances.size());
        for (const Instance& instance : groundInstances) {
            if (!cullingEnabled || frustum.intersects(instance)) {
                visibleGround.push_back(instance.gpu);
            }
        }
        for (const Instance& instance : pillarInstances) {
            if (!cullingEnabled || frustum.intersects(instance)) {
                visiblePillars.push_back(instance.gpu);
            }
        }
        return {std::move(visibleGround), std::move(visiblePillars)};
    }

private:
    void createCubeGeometry(const LoadedMaterials& materials) {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        vertices.reserve(24);
        indices.reserve(36);
        const auto addFace = [&](const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2,
                                 const glm::vec3& p3, const glm::vec3& normal, const glm::vec3& tangent) {
            const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
            vertices.push_back({p0, normal, glm::vec2(0.0f, 0.0f), tangent});
            vertices.push_back({p1, normal, glm::vec2(1.0f, 0.0f), tangent});
            vertices.push_back({p2, normal, glm::vec2(1.0f, 1.0f), tangent});
            vertices.push_back({p3, normal, glm::vec2(0.0f, 1.0f), tangent});
            indices.insert(indices.end(), {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});
        };
        addFace({-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f});
        addFace({0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f});
        addFace({0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
        addFace({-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
        addFace({-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
        addFace({-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f});

        cubeIndexCount = static_cast<GLsizei>(indices.size());
        glGenBuffers(1, &cubeVbo);
        glBindBuffer(GL_ARRAY_BUFFER, cubeVbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STATIC_DRAW);
        glGenBuffers(1, &cubeEbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeEbo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data(), GL_STATIC_DRAW);

        glGenBuffers(1, &sceneInstanceVbo);
        glBindBuffer(GL_ARRAY_BUFFER, sceneInstanceVbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(GpuInstance), nullptr, GL_STREAM_DRAW);
        glGenBuffers(1, &shadowInstanceVbo);

        glGenVertexArrays(1, &sceneVao);
        setupInstancedVao(sceneVao, sceneInstanceVbo);
        glGenVertexArrays(1, &shadowVao);
        setupInstancedVao(shadowVao, shadowInstanceVbo);

        glGenVertexArrays(1, &captureVao);
        glBindVertexArray(captureVao);
        glBindBuffer(GL_ARRAY_BUFFER, cubeVbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeEbo);

        glGenVertexArrays(1, &fullscreenVao);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        if (materials.textureSize != 32) {
            fail("材质 textureSize 在创建几何体时不是 32");
        }
    }

    void setupInstancedVao(GLuint vao, GLuint instanceBuffer) const {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, cubeVbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, uv)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, tangent)));
        glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer);
        for (GLuint column = 0; column < 4; ++column) {
            glEnableVertexAttribArray(4 + column);
            glVertexAttribPointer(4 + column, 4, GL_FLOAT, GL_FALSE, sizeof(GpuInstance),
                                  reinterpret_cast<void*>(sizeof(glm::vec4) * column));
            glVertexAttribDivisor(4 + column, 1);
        }
        glEnableVertexAttribArray(8);
        glVertexAttribIPointer(8, 1, GL_INT, sizeof(GpuInstance), reinterpret_cast<void*>(offsetof(GpuInstance, materialIndex)));
        glVertexAttribDivisor(8, 1);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeEbo);
    }

    void createMaterialTextures(const LoadedMaterials& materials) {
        const GeneratedTextures generated = generateMaterialTextures(materials);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        createArrayTexture(baseColorArray, materials.textureSize, static_cast<GLsizei>(materials.materials.size()),
                           GL_SRGB8_ALPHA8, generated.baseColor.data());
        createArrayTexture(normalArray, materials.textureSize, static_cast<GLsizei>(materials.materials.size()),
                           GL_RGBA8, generated.normal.data());
        createArrayTexture(ormArray, materials.textureSize, static_cast<GLsizei>(materials.materials.size()),
                           GL_RGBA8, generated.orm.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    void createArrayTexture(GLuint& texture, int size, GLsizei layers, GLenum internalFormat, const std::uint8_t* data) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, static_cast<GLint>(internalFormat), size, size, layers, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }

    void createSceneTargets() {
        glGenFramebuffers(1, &sceneFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFbo);
        glGenTextures(1, &sceneColor);
        glBindTexture(GL_TEXTURE_2D, sceneColor);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, kRenderWidth, kRenderHeight, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColor, 0);

        glGenTextures(1, &sceneDepth);
        glBindTexture(GL_TEXTURE_2D, sceneDepth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kRenderWidth, kRenderHeight, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sceneDepth, 0);
        const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);
        checkFramebuffer("主场景");
    }

    void createShadowTargets() {
        glGenFramebuffers(1, &shadowFbo);
        glGenTextures(1, &shadowMap);
        glBindTexture(GL_TEXTURE_2D, shadowMap);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, kShadowResolution, kShadowResolution, 0,
                     GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowMap, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        checkFramebuffer("方向光 shadow");
    }

    void createIblTargets() {
        glGenFramebuffers(1, &captureFbo);
        glGenRenderbuffers(1, &captureDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, captureDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kEnvironmentSize, kEnvironmentSize);
        glBindFramebuffer(GL_FRAMEBUFFER, captureFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, captureDepth);

        glGenTextures(1, &environmentMap);
        glBindTexture(GL_TEXTURE_CUBE_MAP, environmentMap);
        glTexStorage2D(GL_TEXTURE_CUBE_MAP, 9, GL_RGB16F, kEnvironmentSize, kEnvironmentSize);
        setCubeMapFiltering(GL_TEXTURE_CUBE_MAP, GL_LINEAR_MIPMAP_LINEAR);

        glGenTextures(1, &irradianceMap);
        glBindTexture(GL_TEXTURE_CUBE_MAP, irradianceMap);
        glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGB16F, kIrradianceSize, kIrradianceSize);
        setCubeMapFiltering(GL_TEXTURE_CUBE_MAP, GL_LINEAR);

        glGenTextures(1, &prefilterMap);
        glBindTexture(GL_TEXTURE_CUBE_MAP, prefilterMap);
        glTexStorage2D(GL_TEXTURE_CUBE_MAP, kPrefilterLevels, GL_RGB16F, kPrefilterSize, kPrefilterSize);
        setCubeMapFiltering(GL_TEXTURE_CUBE_MAP, GL_LINEAR_MIPMAP_LINEAR);

        glGenTextures(1, &brdfLut);
        glBindTexture(GL_TEXTURE_2D, brdfLut);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, kBrdfSize, kBrdfSize, 0, GL_RG, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        renderIblMaps();
    }

    void setCubeMapFiltering(GLenum target, GLenum minFilter) const {
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    }

    void renderIblMaps() {
        const glm::mat4 projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
        const std::array<glm::vec3, 6> directions = {
            glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
            glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
            glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f)
        };
        const std::array<glm::vec3, 6> ups = {
            glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
            glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f),
            glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)
        };
        std::array<glm::mat4, 6> views{};
        for (std::size_t face = 0; face < views.size(); ++face) {
            views[face] = glm::lookAt(glm::vec3(0.0f), directions[face], ups[face]);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, captureFbo);
        glBindVertexArray(captureVao);
        glDisable(GL_CULL_FACE);
        glEnable(GL_DEPTH_TEST);
        glUseProgram(environmentProgram);
        glUniformMatrix4fv(uniformLocation(environmentProgram, "uProjection"), 1, GL_FALSE, glm::value_ptr(projection));
        for (std::size_t face = 0; face < views.size(); ++face) {
            attachCubeFace(environmentMap, static_cast<int>(face), 0, kEnvironmentSize);
            glViewport(0, 0, kEnvironmentSize, kEnvironmentSize);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glUniformMatrix4fv(uniformLocation(environmentProgram, "uView"), 1, GL_FALSE, glm::value_ptr(views[face]));
            glDrawElements(GL_TRIANGLES, cubeIndexCount, GL_UNSIGNED_INT, nullptr);
        }
        glBindTexture(GL_TEXTURE_CUBE_MAP, environmentMap);
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

        glUseProgram(irradianceProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, environmentMap);
        for (std::size_t face = 0; face < views.size(); ++face) {
            attachCubeFace(irradianceMap, static_cast<int>(face), 0, kIrradianceSize);
            glViewport(0, 0, kIrradianceSize, kIrradianceSize);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glUniformMatrix4fv(uniformLocation(irradianceProgram, "uProjection"), 1, GL_FALSE, glm::value_ptr(projection));
            glUniformMatrix4fv(uniformLocation(irradianceProgram, "uView"), 1, GL_FALSE, glm::value_ptr(views[face]));
            glDrawElements(GL_TRIANGLES, cubeIndexCount, GL_UNSIGNED_INT, nullptr);
        }

        glUseProgram(prefilterProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, environmentMap);
        for (int mip = 0; mip < kPrefilterLevels; ++mip) {
            const int size = kPrefilterSize >> mip;
            const float roughness = static_cast<float>(mip) / static_cast<float>(kPrefilterLevels - 1);
            glViewport(0, 0, size, size);
            glUniform1f(uniformLocation(prefilterProgram, "uRoughness"), roughness);
            glUniformMatrix4fv(uniformLocation(prefilterProgram, "uProjection"), 1, GL_FALSE, glm::value_ptr(projection));
            for (std::size_t face = 0; face < views.size(); ++face) {
                attachCubeFace(prefilterMap, static_cast<int>(face), mip, size);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glUniformMatrix4fv(uniformLocation(prefilterProgram, "uView"), 1, GL_FALSE, glm::value_ptr(views[face]));
                glDrawElements(GL_TRIANGLES, cubeIndexCount, GL_UNSIGNED_INT, nullptr);
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, captureFbo);
        glBindRenderbuffer(GL_RENDERBUFFER, captureDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kBrdfSize, kBrdfSize);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, captureDepth);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, brdfLut, 0);
        const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);
        checkFramebuffer("BRDF LUT");
        glDisable(GL_DEPTH_TEST);
        glUseProgram(brdfProgram);
        glViewport(0, 0, kBrdfSize, kBrdfSize);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindVertexArray(fullscreenVao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindVertexArray(0);
        glUseProgram(0);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    void attachCubeFace(GLuint texture, int face, int level, int size) const {
        glBindRenderbuffer(GL_RENDERBUFFER, captureDepth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, size, size);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, captureDepth);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               static_cast<GLenum>(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face), texture, level);
        const GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &drawBuffer);
        checkFramebuffer("IBL cubemap");
    }

    void createFrameBuffer() {
        glGenBuffers(1, &frameUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, frameUbo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(FrameBlockData), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    void configureSamplers() {
        setSampler(sceneProgram, "uBaseColorTex", 0);
        setSampler(sceneProgram, "uNormalTex", 1);
        setSampler(sceneProgram, "uOrmTex", 2);
        setSampler(sceneProgram, "uIrradianceMap", 3);
        setSampler(sceneProgram, "uPrefilterMap", 4);
        setSampler(sceneProgram, "uBrdfLut", 5);
        setSampler(sceneProgram, "uShadowMap", 6);
        setSampler(irradianceProgram, "uEnvironmentMap", 0);
        setSampler(prefilterProgram, "uEnvironmentMap", 0);
        setSampler(presentProgram, "uSceneColor", 0);
    }

    void buildInstances(const LoadedMaterials& materials) {
        groundInstances.reserve(225);
        pillarInstances.reserve(25);
        for (int x = -7; x <= 7; ++x) {
            for (int z = -7; z <= 7; ++z) {
                Instance instance;
                instance.model = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(x), 0.5f, static_cast<float>(z)));
                instance.boundsMin = glm::vec3(x - 0.5f, 0.0f, z - 0.5f);
                instance.boundsMax = glm::vec3(x + 0.5f, 1.0f, z + 0.5f);
                instance.gpu.model = instance.model;
                instance.gpu.materialIndex = materials.groundLayer;
                groundInstances.push_back(instance);
            }
        }
        const std::array<glm::ivec2, 5> pillarPositions = {
            glm::ivec2(-7, -7), glm::ivec2(-7, 7), glm::ivec2(7, -7), glm::ivec2(7, 7), glm::ivec2(0, 0)
        };
        for (const glm::ivec2 position : pillarPositions) {
            for (int layer = 0; layer < 5; ++layer) {
                Instance instance;
                instance.model = glm::translate(glm::mat4(1.0f),
                                                glm::vec3(static_cast<float>(position.x), 1.5f + layer,
                                                          static_cast<float>(position.y)));
                instance.boundsMin = glm::vec3(position.x - 0.5f, 1.0f + layer, position.y - 0.5f);
                instance.boundsMax = glm::vec3(position.x + 0.5f, 2.0f + layer, position.y + 0.5f);
                instance.gpu.model = instance.model;
                instance.gpu.materialIndex = materials.pillarLayer;
                pillarInstances.push_back(instance);
            }
        }
        if (groundInstances.size() != 225 || pillarInstances.size() != 25) {
            fail("场景实例数量错误");
        }
        shadowInstances.reserve(kExpectedInstanceCount);
        for (const Instance& instance : groundInstances) {
            shadowInstances.push_back(instance.gpu);
        }
        for (const Instance& instance : pillarInstances) {
            shadowInstances.push_back(instance.gpu);
        }
        if (shadowInstances.size() != kExpectedInstanceCount) {
            fail("shadow 实例数量不是 250");
        }
        glBindBuffer(GL_ARRAY_BUFFER, shadowInstanceVbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(shadowInstances.size() * sizeof(GpuInstance)),
                     shadowInstances.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void uploadVisible(const std::vector<GpuInstance>& instances) const {
        glBindBuffer(GL_ARRAY_BUFFER, sceneInstanceVbo);
        const std::size_t count = std::max<std::size_t>(instances.size(), 1);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(GpuInstance)), nullptr, GL_STREAM_DRAW);
        if (!instances.empty()) {
            glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(instances.size() * sizeof(GpuInstance)), instances.data());
        }
    }
};

}

int main() {
    GLFWwindow* window = nullptr;
    bool glfwReady = false;
    Renderer renderer;
    try {
        const fs::path root = findProjectRoot();
        const LoadedMaterials materials = loadMaterials(root);
        std::cout << "资源根目录: " << pathText(root) << std::endl;

        if (glfwInit() != GLFW_TRUE) {
            fail("GLFW 初始化失败");
        }
        glfwReady = true;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_SAMPLES, 0);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window = glfwCreateWindow(kWindowWidth, kWindowHeight, "OpenGL VoxelPBRFrustumCulling Test", nullptr, nullptr);
        if (window == nullptr) {
            fail("无法创建 1280 x 720 GLFW 窗口或 OpenGL 4.6 上下文");
        }
        glfwMakeContextCurrent(window);
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
            fail("GLAD 加载 OpenGL 函数失败");
        }
        renderer.contextReady = true;
        GLint glMajor = 0;
        GLint glMinor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &glMajor);
        glGetIntegerv(GL_MINOR_VERSION, &glMinor);
        if (glMajor < 4 || (glMajor == 4 && glMinor < 6)) {
            fail("实际 OpenGL 版本低于 4.6: " + std::to_string(glMajor) + "." + std::to_string(glMinor));
        }
        std::cout << "GPU vendor: " << reinterpret_cast<const char*>(glGetString(GL_VENDOR)) << std::endl;
        std::cout << "GPU renderer: " << reinterpret_cast<const char*>(glGetString(GL_RENDERER)) << std::endl;
        std::cout << "OpenGL version: " << reinterpret_cast<const char*>(glGetString(GL_VERSION)) << std::endl;
        std::cout << "GLSL version: " << reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)) << std::endl;

        AppState state;
        state.window = window;
        glfwSetWindowUserPointer(window, &state);
        glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
        glfwSetCursorPosCallback(window, cursorPositionCallback);
        glfwSetKeyCallback(window, keyCallback);
        glfwSetMouseButtonCallback(window, mouseButtonCallback);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        glfwGetFramebufferSize(window, &state.framebufferWidth, &state.framebufferHeight);
        glfwSwapInterval(1);

        renderer.initialize(root, materials);
        const glm::vec3 sunDirection = glm::normalize(glm::vec3(-0.45f, -1.0f, -0.35f));
        const glm::vec3 sunRadiance(4.5f, 4.2f, 3.8f);
        const glm::vec3 lightTarget(0.0f, 2.0f, 0.0f);
        const glm::vec3 lightPosition = lightTarget - sunDirection * 35.0f;
        const glm::mat4 lightView = glm::lookAt(lightPosition, lightTarget, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 lightProjection = glm::ortho(-18.0f, 18.0f, -18.0f, 18.0f, -20.0f, 80.0f);
        const glm::mat4 lightSpace = lightProjection * lightView;
        const std::array<glm::vec3, 4> pointPositions = {
            glm::vec3(-5.0f, 6.5f, -5.0f), glm::vec3(-5.0f, 6.5f, 5.0f),
            glm::vec3(5.0f, 6.5f, -5.0f), glm::vec3(5.0f, 6.5f, 5.0f)
        };
        const std::array<glm::vec3, 4> pointColors = {
            glm::vec3(1.0f, 0.55f, 0.28f), glm::vec3(0.28f, 0.55f, 1.0f),
            glm::vec3(0.28f, 0.55f, 1.0f), glm::vec3(1.0f, 0.55f, 0.28f)
        };

        double previousTime = glfwGetTime();
        double titleTime = previousTime;
        int framesSinceTitle = 0;
        int lastVisibleCount = kExpectedInstanceCount;
        int lastDrawCalls = 2;
        while (glfwWindowShouldClose(window) == GLFW_FALSE) {
            const double currentTime = glfwGetTime();
            const float deltaTime = std::clamp(static_cast<float>(currentTime - previousTime), 0.0f, 0.1f);
            previousTime = currentTime;
            glfwPollEvents();
            updateCamera(state, deltaTime);

            if (state.framebufferWidth <= 0 || state.framebufferHeight <= 0) {
                glfwWaitEventsTimeout(0.05);
                continue;
            }

            const glm::mat4 projection = glm::perspective(glm::radians(60.0f),
                                                           static_cast<float>(kRenderWidth) / static_cast<float>(kRenderHeight),
                                                           0.1f, 100.0f);
            const glm::mat4 view = state.camera.viewMatrix();
            const glm::mat4 viewProjection = projection * view;
            const Frustum frustum = Frustum::fromMatrix(viewProjection);
            auto visible = renderer.visibleInstances(frustum, state.cullingEnabled);
            lastVisibleCount = static_cast<int>(visible.first.size() + visible.second.size());

            renderer.renderShadow(lightSpace);
            FrameBlockData frame;
            frame.viewProjection = viewProjection;
            frame.lightSpace = lightSpace;
            frame.cameraPosition = glm::vec4(state.camera.position, 1.0f);
            frame.sunDirection = glm::vec4(sunDirection, 0.0f);
            frame.sunRadiance = glm::vec4(sunRadiance, 0.0f);
            for (std::size_t index = 0; index < pointPositions.size(); ++index) {
                frame.pointPositions[index] = glm::vec4(pointPositions[index], 1.0f);
                frame.pointColors[index] = glm::vec4(pointColors[index] * 35.0f, 0.0f);
            }
            lastDrawCalls = renderer.renderMain(frame, visible.first, visible.second, state.debugMode);
            renderer.present(state);
            glfwSwapBuffers(window);

            ++framesSinceTitle;
            if (currentTime - titleTime >= 1.0) {
                const double fps = static_cast<double>(framesSinceTitle) / (currentTime - titleTime);
                titleTime = currentTime;
                framesSinceTitle = 0;
                std::ostringstream title;
                title << "OpenGL VoxelPBRFrustumCulling Test | FPS: " << std::fixed << std::setprecision(1) << fps
                      << " | Visible: " << lastVisibleCount << "/250 | Culled: " << (kExpectedInstanceCount - lastVisibleCount)
                      << " | Cube draws: " << lastDrawCalls << " | Debug: " << state.debugMode;
                glfwSetWindowTitle(window, title.str().c_str());
            }
        }

        renderer.destroy();
        glfwDestroyWindow(window);
        window = nullptr;
        glfwTerminate();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "程序安全退出: " << error.what() << std::endl;
        renderer.destroy();
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        if (glfwReady) {
            glfwTerminate();
        }
        return EXIT_FAILURE;
    }
}