#define GLFW_INCLUDE_VULKAN
#include <volk.h>
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "shaders.inc"

#if defined(_WIN32)
#pragma comment(lib, "glfw3dll.lib")
#pragma comment(lib, "vulkan-1.lib")
#pragma comment(lib, "volk.lib")
#endif

namespace {

using Json = nlohmann::json;

constexpr uint32_t kFramesInFlight = 2;
constexpr float kPi = 3.14159265358979323846f;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string vkResultText(VkResult result) {
    std::ostringstream stream;
    stream << static_cast<int>(result);
    return stream.str();
}

void checkVk(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) {
        fail(std::string(operation) + " 失败，VkResult=" + vkResultText(result));
    }
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

uint8_t toByte(float value) {
    return static_cast<uint8_t>(std::lround(clamp01(value) * 255.0f));
}

struct Vec2 {
    float x{};
    float y{};
};

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator-(Vec3 value) {
    return {-value.x, -value.y, -value.z};
}

Vec3 operator*(Vec3 value, float scalar) {
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

Vec3 operator*(float scalar, Vec3 value) {
    return value * scalar;
}

float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

Vec3 normalize(Vec3 value) {
    const float lengthSquared = dot(value, value);
    if (lengthSquared <= std::numeric_limits<float>::epsilon()) {
        return {0.0f, 0.0f, 0.0f};
    }
    return value * (1.0f / std::sqrt(lengthSquared));
}

struct Mat4 {
    std::array<float, 16> values{};
};

Mat4 identityMatrix() {
    Mat4 result{};
    result.values[0] = 1.0f;
    result.values[5] = 1.0f;
    result.values[10] = 1.0f;
    result.values[15] = 1.0f;
    return result;
}

Mat4 multiply(const Mat4& left, const Mat4& right) {
    Mat4 result{};
    for (uint32_t column = 0; column < 4; ++column) {
        for (uint32_t row = 0; row < 4; ++row) {
            float value = 0.0f;
            for (uint32_t index = 0; index < 4; ++index) {
                value += left.values[index * 4 + row] * right.values[column * 4 + index];
            }
            result.values[column * 4 + row] = value;
        }
    }
    return result;
}

Mat4 translationMatrix(Vec3 position) {
    Mat4 result = identityMatrix();
    result.values[12] = position.x;
    result.values[13] = position.y;
    result.values[14] = position.z;
    return result;
}

Mat4 perspectiveMatrix(float verticalFieldOfView, float aspect, float nearPlane, float farPlane) {
    const float scale = 1.0f / std::tan(verticalFieldOfView * 0.5f);
    Mat4 result{};
    result.values[0] = scale / aspect;
    result.values[5] = -scale;
    result.values[10] = farPlane / (nearPlane - farPlane);
    result.values[11] = -1.0f;
    result.values[14] = (farPlane * nearPlane) / (nearPlane - farPlane);
    return result;
}

Mat4 lookAtMatrix(Vec3 eye, Vec3 center, Vec3 up) {
    const Vec3 forward = normalize(center - eye);
    const Vec3 side = normalize(cross(forward, up));
    const Vec3 correctedUp = cross(side, forward);

    Mat4 result = identityMatrix();
    result.values[0] = side.x;
    result.values[4] = side.y;
    result.values[8] = side.z;
    result.values[12] = -dot(side, eye);
    result.values[1] = correctedUp.x;
    result.values[5] = correctedUp.y;
    result.values[9] = correctedUp.z;
    result.values[13] = -dot(correctedUp, eye);
    result.values[2] = -forward.x;
    result.values[6] = -forward.y;
    result.values[10] = -forward.z;
    result.values[14] = dot(forward, eye);
    return result;
}

uint32_t hashBits(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

float random01(uint32_t seed, uint32_t x, uint32_t y, uint32_t channel) {
    uint32_t value = seed;
    value ^= hashBits(x + 0x9e3779b9U);
    value ^= hashBits(y + 0x85ebca6bU);
    value ^= hashBits(channel + 0xc2b2ae35U);
    return static_cast<float>(hashBits(value)) / static_cast<float>(std::numeric_limits<uint32_t>::max());
}

float interpolate(float a, float b, float amount) {
    return a + (b - a) * amount;
}

float smoothStep(float value) {
    return value * value * (3.0f - 2.0f * value);
}

float valueNoise(uint32_t seed, float x, float y) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = smoothStep(x - static_cast<float>(x0));
    const float fy = smoothStep(y - static_cast<float>(y0));
    const auto sample = [seed](int sx, int sy) {
        return random01(seed, static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), 0x51f15eU);
    };
    const float top = interpolate(sample(x0, y0), sample(x0 + 1, y0), fx);
    const float bottom = interpolate(sample(x0, y0 + 1), sample(x0 + 1, y0 + 1), fx);
    return interpolate(top, bottom, fy);
}

struct MaterialConfig {
    std::string id;
    std::array<float, 3> baseColorSRGB{};
    float metallic{};
    float roughness{};
    float ambientOcclusion{};
    float normalStrength{};
    float baseColorVariation{};
    float roughnessVariation{};
    uint32_t textureSeed{};
    std::string pattern;
};

struct MaterialConfigSet {
    uint32_t textureSize{};
    std::vector<MaterialConfig> materials;
    std::unordered_map<std::string, uint32_t> indexById;
};

const Json& requiredMember(const Json& object, const char* name, const std::string& context) {
    if (!object.contains(name)) {
        fail("配置错误：" + context + " 缺少字段 " + name);
    }
    return object.at(name);
}

double readFiniteNumber(const Json& value, const std::string& context, double minimum, double maximum) {
    if (!value.is_number()) {
        fail("配置错误：" + context + " 必须是数字");
    }
    const double number = value.get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum) {
        fail("配置错误：" + context + " 超出合法范围");
    }
    return number;
}

MaterialConfigSet loadMaterialConfig() {
    std::ifstream file("materials.json");
    if (!file) {
        fail("配置错误：无法打开当前目录的 materials.json");
    }

    Json root;
    try {
        file >> root;
    } catch (const Json::exception& exception) {
        fail(std::string("配置错误：materials.json 解析失败：") + exception.what());
    }
    if (!root.is_object()) {
        fail("配置错误：materials.json 根节点必须是对象");
    }

    const Json& schemaVersion = requiredMember(root, "schemaVersion", "根节点");
    if (!schemaVersion.is_number_integer() || schemaVersion.get<int64_t>() != 1) {
        fail("配置错误：不支持的 schemaVersion，仅支持版本 1");
    }

    const Json& textureSizeValue = requiredMember(root, "textureSize", "根节点");
    if (!textureSizeValue.is_number_integer()) {
        fail("配置错误：textureSize 必须是正整数");
    }
    const int64_t textureSizeSigned = textureSizeValue.get<int64_t>();
    if (textureSizeSigned <= 0 || static_cast<uint64_t>(textureSizeSigned) > std::numeric_limits<uint32_t>::max()) {
        fail("配置错误：textureSize 必须在正整数范围内");
    }

    const Json& materialArray = requiredMember(root, "materials", "根节点");
    if (!materialArray.is_array() || materialArray.empty()) {
        fail("配置错误：materials 必须是非空数组");
    }

    MaterialConfigSet result;
    result.textureSize = static_cast<uint32_t>(textureSizeSigned);
    result.materials.reserve(materialArray.size());

    for (size_t index = 0; index < materialArray.size(); ++index) {
        const std::string context = "materials[" + std::to_string(index) + "]";
        const Json& material = materialArray.at(index);
        if (!material.is_object()) {
            fail("配置错误：" + context + " 必须是对象");
        }

        MaterialConfig config;
        const Json& id = requiredMember(material, "id", context);
        if (!id.is_string() || id.get<std::string>().empty()) {
            fail("配置错误：" + context + ".id 必须是非空字符串");
        }
        config.id = id.get<std::string>();
        if (result.indexById.contains(config.id)) {
            fail("配置错误：材质 ID 重复：" + config.id);
        }

        const Json& baseColor = requiredMember(material, "baseColorSRGB", context);
        if (!baseColor.is_array() || baseColor.size() != config.baseColorSRGB.size()) {
            fail("配置错误：" + context + ".baseColorSRGB 必须是长度为 3 的数组");
        }
        for (size_t component = 0; component < config.baseColorSRGB.size(); ++component) {
            config.baseColorSRGB[component] = static_cast<float>(readFiniteNumber(
                baseColor.at(component), context + ".baseColorSRGB[" + std::to_string(component) + "]", 0.0, 1.0));
        }

        config.metallic = static_cast<float>(readFiniteNumber(requiredMember(material, "metallic", context), context + ".metallic", 0.0, 1.0));
        config.roughness = static_cast<float>(readFiniteNumber(requiredMember(material, "roughness", context), context + ".roughness", 0.0, 1.0));
        config.ambientOcclusion = static_cast<float>(readFiniteNumber(requiredMember(material, "ambientOcclusion", context), context + ".ambientOcclusion", 0.0, 1.0));
        config.normalStrength = static_cast<float>(readFiniteNumber(requiredMember(material, "normalStrength", context), context + ".normalStrength", 0.0, 1.0));
        config.baseColorVariation = static_cast<float>(readFiniteNumber(requiredMember(material, "baseColorVariation", context), context + ".baseColorVariation", 0.0, 1.0));
        config.roughnessVariation = static_cast<float>(readFiniteNumber(requiredMember(material, "roughnessVariation", context), context + ".roughnessVariation", 0.0, 1.0));

        const Json& textureSeed = requiredMember(material, "textureSeed", context);
        if (!textureSeed.is_number_integer()) {
            fail("配置错误：" + context + ".textureSeed 必须是非负整数");
        }
        const int64_t seedSigned = textureSeed.get<int64_t>();
        if (seedSigned < 0 || static_cast<uint64_t>(seedSigned) > std::numeric_limits<uint32_t>::max()) {
            fail("配置错误：" + context + ".textureSeed 超出 uint32 范围");
        }
        config.textureSeed = static_cast<uint32_t>(seedSigned);

        const Json& pattern = requiredMember(material, "pattern", context);
        if (!pattern.is_string()) {
            fail("配置错误：" + context + ".pattern 必须是字符串");
        }
        config.pattern = pattern.get<std::string>();
        if (config.pattern != "brushed_x" && config.pattern != "molded") {
            fail("配置错误：" + context + ".pattern 不受支持：" + config.pattern);
        }

        result.indexById.emplace(config.id, static_cast<uint32_t>(result.materials.size()));
        result.materials.push_back(std::move(config));
    }
    if (!result.indexById.contains("brushed_metal") || !result.indexById.contains("red_plastic")) {
        fail("配置错误：场景需要 brushed_metal 和 red_plastic 两个材质 ID");
    }
    return result;
}

struct GeneratedTextures {
    std::vector<uint8_t> baseColor;
    std::vector<uint8_t> roughness;
    std::vector<uint8_t> normal;
};

GeneratedTextures generateTextures(const MaterialConfig& material, uint32_t textureSize) {
    const uint64_t pixelCount64 = static_cast<uint64_t>(textureSize) * textureSize;
    if (pixelCount64 > std::numeric_limits<size_t>::max() / 4) {
        fail("配置错误：textureSize 导致纹理内存大小溢出");
    }
    const size_t byteCount = static_cast<size_t>(pixelCount64) * 4;
    GeneratedTextures result;
    result.baseColor.resize(byteCount);
    result.roughness.resize(byteCount);
    result.normal.resize(byteCount);
    std::vector<float> heights(static_cast<size_t>(pixelCount64));

    for (uint32_t y = 0; y < textureSize; ++y) {
        for (uint32_t x = 0; x < textureSize; ++x) {
            const size_t pixel = static_cast<size_t>(y) * textureSize + x;
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(textureSize);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(textureSize);
            const float fine = random01(material.textureSeed, x, y, 0x13U);
            float height = 0.0f;
            if (material.pattern == "brushed_x") {
                const float lowFrequency = valueNoise(material.textureSeed ^ 0x68bc21ebU, u * 6.0f, v * 6.0f);
                const float stripe = 0.5f + 0.5f * std::sin((u * static_cast<float>(textureSize) * 0.75f) + (fine - 0.5f) * 0.7f);
                height = (stripe - 0.5f) * 0.8f + (lowFrequency - 0.5f) * 0.55f + (fine - 0.5f) * 0.08f;
            } else {
                const float grain = valueNoise(material.textureSeed ^ 0x9d2c5680U, u * 22.0f, v * 22.0f);
                const float particles = random01(material.textureSeed, x, y, 0xa7U);
                height = (grain - 0.5f) * 0.42f + (particles - 0.5f) * 0.24f;
            }
            heights[pixel] = height;

            const float colorVariation = height + (fine - 0.5f) * 0.35f;
            const float roughnessVariation = height + (fine - 0.5f) * 0.5f;
            const size_t byteOffset = pixel * 4;
            for (size_t component = 0; component < 3; ++component) {
                result.baseColor[byteOffset + component] = toByte(
                    material.baseColorSRGB[component] + material.baseColorVariation * colorVariation);
            }
            result.baseColor[byteOffset + 3] = 255;
            const uint8_t roughness = toByte(material.roughness + material.roughnessVariation * roughnessVariation);
            result.roughness[byteOffset + 0] = roughness;
            result.roughness[byteOffset + 1] = roughness;
            result.roughness[byteOffset + 2] = roughness;
            result.roughness[byteOffset + 3] = 255;
        }
    }

    for (uint32_t y = 0; y < textureSize; ++y) {
        for (uint32_t x = 0; x < textureSize; ++x) {
            const uint32_t left = (x + textureSize - 1) % textureSize;
            const uint32_t right = (x + 1) % textureSize;
            const uint32_t down = (y + textureSize - 1) % textureSize;
            const uint32_t up = (y + 1) % textureSize;
            const size_t pixel = static_cast<size_t>(y) * textureSize + x;
            const float dx = heights[static_cast<size_t>(y) * textureSize + right] - heights[static_cast<size_t>(y) * textureSize + left];
            const float dy = heights[static_cast<size_t>(up) * textureSize + x] - heights[static_cast<size_t>(down) * textureSize + x];
            const Vec3 normal = normalize({-dx * material.normalStrength * 2.0f, -dy * material.normalStrength * 2.0f, 1.0f});
            const size_t byteOffset = pixel * 4;
            result.normal[byteOffset + 0] = toByte(normal.x * 0.5f + 0.5f);
            result.normal[byteOffset + 1] = toByte(normal.y * 0.5f + 0.5f);
            result.normal[byteOffset + 2] = toByte(normal.z * 0.5f + 0.5f);
            result.normal[byteOffset + 3] = 255;
        }
    }
    return result;
}

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec3 tangent;
    Vec2 uv;
};

struct SceneObject {
    Vec3 position;
    uint32_t materialIndex{};
};

std::vector<Vertex> makeCubeVertices(std::vector<uint32_t>& indices) {
    struct Face {
        Vec3 normal;
        Vec3 tangent;
        Vec3 bitangent;
    };
    const std::array<Face, 6> faces = {{
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
        {{0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    }};
    const std::array<Vec2, 4> uvs = {{{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};
    std::vector<Vertex> vertices;
    vertices.reserve(faces.size() * 4);
    indices.clear();
    indices.reserve(faces.size() * 6);
    for (const Face& face : faces) {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (const Vec2& uv : uvs) {
            const Vec3 position = face.normal * 0.5f + face.tangent * ((uv.x - 0.5f)) + face.bitangent * ((uv.y - 0.5f));
            vertices.push_back({position, face.normal, face.tangent, uv});
        }
        indices.insert(indices.end(), {base + 0, base + 1, base + 2, base + 2, base + 3, base + 0});
    }
    return vertices;
}

struct BufferResource {
    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
};

struct ImageResource {
    VkImage image{VK_NULL_HANDLE};
    VkDeviceMemory memory{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
};

struct RuntimeMaterial {
    MaterialConfig config;
    ImageResource baseColor;
    ImageResource roughness;
    ImageResource normal;
};

struct PushConstants {
    Mat4 viewProjection;
    Mat4 model;
    std::array<float, 4> material;
    std::array<float, 4> cameraPosition;
    std::array<float, 4> lightDirection;
};

static_assert(sizeof(PushConstants) == 176);




class RubikCubeApp {
public:
    RubikCubeApp() {
        config = loadMaterialConfig();
        initializeWindow();
        initializeVulkan();
    }

    ~RubikCubeApp() {
        cleanup();
    }

    RubikCubeApp(const RubikCubeApp&) = delete;
    RubikCubeApp& operator=(const RubikCubeApp&) = delete;

    void run() {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            drawFrame();
        }
        if (device != VK_NULL_HANDLE) {
            checkVk(vkDeviceWaitIdle(device), "等待设备空闲");
        }
    }

private:
    struct QueueFamilySelection {
        uint32_t graphicsAndPresent{};
        std::optional<uint32_t> transfer;
    };

    struct FrameResources {
        VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
        VkSemaphore imageAvailable{VK_NULL_HANDLE};
        VkSemaphore renderFinished{VK_NULL_HANDLE};
        VkFence inFlight{VK_NULL_HANDLE};
    };

    MaterialConfigSet config;
    std::vector<RuntimeMaterial> materials;
    std::vector<SceneObject> scene;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    BufferResource vertexBuffer;
    BufferResource indexBuffer;

    GLFWwindow* window{nullptr};
    bool glfwInitialized{false};
    bool volkInitialized{false};
    bool framebufferResized{false};

    VkInstance instance{VK_NULL_HANDLE};
    VkSurfaceKHR surface{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue graphicsQueue{VK_NULL_HANDLE};
    uint32_t graphicsQueueFamily{};
    VkQueue transferQueue{VK_NULL_HANDLE};
    uint32_t transferQueueFamily{};
    bool useHostImageCopy{false};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceProperties deviceProperties{};
    VkImageLayout hostCopyLayout{VK_IMAGE_LAYOUT_GENERAL};

    VkCommandPool commandPool{VK_NULL_HANDLE};
    VkCommandPool transferCommandPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptorSetLayout{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VkPipeline graphicsPipeline{VK_NULL_HANDLE};
    VkSampler sampler{VK_NULL_HANDLE};

    VkSwapchainKHR swapchain{VK_NULL_HANDLE};
    VkFormat swapchainFormat{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkImageLayout> swapchainImageLayouts;
    VkFormat depthFormat{VK_FORMAT_UNDEFINED};
    ImageResource depthImage;
    bool depthNeedsTransition{true};

    std::array<FrameResources, kFramesInFlight> frames{};
    std::vector<VkFence> imagesInFlight;
    size_t currentFrame{};

    static void framebufferResizeCallback(GLFWwindow* resizedWindow, int, int) {
        if (auto* application = static_cast<RubikCubeApp*>(glfwGetWindowUserPointer(resizedWindow))) {
            application->framebufferResized = true;
        }
    }

    void initializeWindow() {
        if (glfwInit() != GLFW_TRUE) {
            fail("GLFW 初始化失败");
        }
        glfwInitialized = true;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        window = glfwCreateWindow(1280, 720, "Vulkan RubikCube", nullptr, nullptr);
        if (window == nullptr) {
            fail("GLFW 窗口创建失败");
        }
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
    }

    void initializeVulkan() {
        initializeInstance();
        checkVk(glfwCreateWindowSurface(instance, window, nullptr, &surface), "创建 Vulkan Surface");
        selectPhysicalDevice();
        createDevice();
        createCommandPool();
        createDescriptorResources();
        createGeometry();
        createTextures();
        createSwapchain();
        createGraphicsPipeline();
        createFrameResources();
        buildScene();
    }

    void initializeInstance() {
        checkVk(volkInitialize(), "加载 Vulkan Loader");
        volkInitialized = true;
        if (volkGetInstanceVersion() < VK_API_VERSION_1_4) {
            fail("Vulkan Loader 不支持 Vulkan 1.4");
        }

        uint32_t extensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&extensionCount);
        if (glfwExtensions == nullptr || extensionCount == 0) {
            fail("GLFW 未提供创建 Vulkan Surface 所需的实例扩展");
        }
        VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        applicationInfo.pApplicationName = "Vulkan RubikCube";
        applicationInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.pEngineName = "RenderArena";
        applicationInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        applicationInfo.apiVersion = VK_API_VERSION_1_4;

        VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        createInfo.pApplicationInfo = &applicationInfo;
        createInfo.enabledExtensionCount = extensionCount;
        createInfo.ppEnabledExtensionNames = glfwExtensions;
        checkVk(vkCreateInstance(&createInfo, nullptr, &instance), "创建 Vulkan 1.4 实例");
        volkLoadInstance(instance);
    }

    static bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
        return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
            return std::strcmp(extension.extensionName, name) == 0;
        });
    }

    std::optional<QueueFamilySelection> findQueueFamily(VkPhysicalDevice candidate) const {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());

        std::optional<uint32_t> graphicsAndPresent;
        for (uint32_t index = 0; index < count; ++index) {
            VkBool32 present = VK_FALSE;
            checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &present), "查询 Surface 队列支持");
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present == VK_TRUE) {
                graphicsAndPresent = index;
                break;
            }
        }
        if (!graphicsAndPresent) {
            return std::nullopt;
        }

        std::optional<uint32_t> dedicatedTransfer;
        std::optional<uint32_t> additionalTransfer;
        for (uint32_t index = 0; index < count; ++index) {
            if (index == *graphicsAndPresent || (families[index].queueFlags & VK_QUEUE_TRANSFER_BIT) == 0) {
                continue;
            }
            if ((families[index].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0) {
                dedicatedTransfer = index;
                break;
            }
            if (!additionalTransfer) {
                additionalTransfer = index;
            }
        }
        return QueueFamilySelection{*graphicsAndPresent, dedicatedTransfer ? dedicatedTransfer : additionalTransfer};
    }

    void selectPhysicalDevice() {
        uint32_t count = 0;
        checkVk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "枚举物理设备");
        if (count == 0) {
            fail("没有可用的 Vulkan 物理设备");
        }
        std::vector<VkPhysicalDevice> devices(count);
        checkVk(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "读取物理设备列表");

        int bestScore = -1;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_4) {
                continue;
            }
            auto queueFamily = findQueueFamily(candidate);
            if (!queueFamily) {
                continue;
            }
            uint32_t extensionCount = 0;
            checkVk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, nullptr), "枚举设备扩展");
            std::vector<VkExtensionProperties> extensions(extensionCount);
            checkVk(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extensionCount, extensions.data()), "读取设备扩展");
            if (!hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
                !hasExtension(extensions, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
                continue;
            }

            VkPhysicalDeviceVulkan14Features features14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
            VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            features13.pNext = &features14;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &features13;
            vkGetPhysicalDeviceFeatures2(candidate, &features);
            const bool hostImageCopySupported = features14.hostImageCopy == VK_TRUE;
            if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE ||
                features14.pushDescriptor != VK_TRUE || (!hostImageCopySupported && !queueFamily->transfer)) {
                continue;
            }

            const int score = (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100000 : 0) +
                              static_cast<int>(properties.limits.maxImageDimension2D);
            if (score > bestScore) {
                bestScore = score;
                physicalDevice = candidate;
                graphicsQueueFamily = queueFamily->graphicsAndPresent;
                transferQueueFamily = queueFamily->transfer.value_or(graphicsQueueFamily);
                useHostImageCopy = hostImageCopySupported;
                deviceProperties = properties;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            fail("没有同时支持 Vulkan 1.4、Dynamic Rendering、Synchronization 2、Push Descriptors 和 Host Image Copy 或独立 Transfer Queue 的物理设备");
        }
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        if (useHostImageCopy) {
            queryHostCopyLayout();
        }
    }

    void queryHostCopyLayout() {
        VkPhysicalDeviceHostImageCopyProperties properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &properties;
        vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
        if (properties.copyDstLayoutCount == 0) {
            fail("物理设备未提供 Host Image Copy 目标图像布局");
        }
        std::vector<VkImageLayout> layouts(properties.copyDstLayoutCount);
        properties.pCopyDstLayouts = layouts.data();
        vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
        const auto general = std::find(layouts.begin(), layouts.end(), VK_IMAGE_LAYOUT_GENERAL);
        if (general != layouts.end()) {
            hostCopyLayout = VK_IMAGE_LAYOUT_GENERAL;
        } else {
            const auto transfer = std::find(layouts.begin(), layouts.end(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            hostCopyLayout = transfer != layouts.end() ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : layouts.front();
        }
    }

    void createDevice() {
        const float queuePriority = 1.0f;
        std::array<VkDeviceQueueCreateInfo, 2> queueInfos{};
        queueInfos[0] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfos[0].queueFamilyIndex = graphicsQueueFamily;
        queueInfos[0].queueCount = 1;
        queueInfos[0].pQueuePriorities = &queuePriority;
        uint32_t queueInfoCount = 1;
        if (!useHostImageCopy) {
            queueInfos[1] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queueInfos[1].queueFamilyIndex = transferQueueFamily;
            queueInfos[1].queueCount = 1;
            queueInfos[1].pQueuePriorities = &queuePriority;
            queueInfoCount = 2;
        }

        const std::array<const char*, 2> deviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        };
        VkPhysicalDeviceVulkan14Features features14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
        features14.hostImageCopy = useHostImageCopy ? VK_TRUE : VK_FALSE;
        features14.pushDescriptor = VK_TRUE;
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.synchronization2 = VK_TRUE;
        features13.dynamicRendering = VK_TRUE;
        features13.pNext = &features14;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &features13;

        VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        createInfo.pNext = &features;
        createInfo.queueCreateInfoCount = queueInfoCount;
        createInfo.pQueueCreateInfos = queueInfos.data();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();
        checkVk(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device), "创建 Vulkan 设备");
        volkLoadDevice(device);
        vkGetDeviceQueue(device, graphicsQueueFamily, 0, &graphicsQueue);
        if (!useHostImageCopy) {
            vkGetDeviceQueue(device, transferQueueFamily, 0, &transferQueue);
        }
    }

    void createCommandPool() {
        VkCommandPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        createInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        createInfo.queueFamilyIndex = graphicsQueueFamily;
        checkVk(vkCreateCommandPool(device, &createInfo, nullptr, &commandPool), "创建命令池");
        if (!useHostImageCopy) {
            createInfo.queueFamilyIndex = transferQueueFamily;
            checkVk(vkCreateCommandPool(device, &createInfo, nullptr, &transferCommandPool), "创建传输命令池");
        }
    }

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
        for (uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((typeFilter & (1U << index)) != 0 &&
                (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties) {
                return index;
            }
        }
        fail("找不到满足 Vulkan 内存属性的内存类型");
    }

    void destroyBuffer(BufferResource& buffer) {
        if (buffer.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer.buffer, nullptr);
        }
        if (buffer.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, buffer.memory, nullptr);
        }
        buffer = {};
    }

    void destroyImage(ImageResource& image) {
        if (image.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, image.view, nullptr);
        }
        if (image.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image.image, nullptr);
        }
        if (image.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, image.memory, nullptr);
        }
        image = {};
    }

    BufferResource createHostBuffer(const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
        BufferResource resource{};
        try {
            VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bufferInfo.size = size;
            bufferInfo.usage = usage;
            bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            checkVk(vkCreateBuffer(device, &bufferInfo, nullptr, &resource.buffer), "创建几何缓冲");

            VkMemoryRequirements requirements{};
            vkGetBufferMemoryRequirements(device, resource.buffer, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryType(
                requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            checkVk(vkAllocateMemory(device, &allocation, nullptr, &resource.memory), "分配几何缓冲内存");
            checkVk(vkBindBufferMemory(device, resource.buffer, resource.memory, 0), "绑定几何缓冲内存");

            void* mapped = nullptr;
            checkVk(vkMapMemory(device, resource.memory, 0, size, 0, &mapped), "映射几何缓冲内存");
            std::memcpy(mapped, data, static_cast<size_t>(size));
            vkUnmapMemory(device, resource.memory);
            return resource;
        } catch (...) {
            destroyBuffer(resource);
            throw;
        }
    }

    ImageResource createImage(VkFormat format, VkExtent3D extent, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                              VkMemoryPropertyFlags requiredMemoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              bool shareWithTransferQueue = false) {
        ImageResource resource{};
        resource.format = format;
        try {
            std::array<uint32_t, 2> sharingFamilies = {graphicsQueueFamily, transferQueueFamily};
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = extent;
            imageInfo.mipLevels = 1;
            imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = usage;
            imageInfo.sharingMode = shareWithTransferQueue ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
            if (shareWithTransferQueue) {
                imageInfo.queueFamilyIndexCount = 2;
                imageInfo.pQueueFamilyIndices = sharingFamilies.data();
            }
            imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            checkVk(vkCreateImage(device, &imageInfo, nullptr, &resource.image), "创建 Vulkan 图像");

            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, resource.image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = findMemoryType(requirements.memoryTypeBits, requiredMemoryProperties);
            checkVk(vkAllocateMemory(device, &allocation, nullptr, &resource.memory), "分配 Vulkan 图像内存");
            checkVk(vkBindImageMemory(device, resource.image, resource.memory, 0), "绑定 Vulkan 图像内存");

            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = resource.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = format;
            viewInfo.subresourceRange.aspectMask = aspect;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            checkVk(vkCreateImageView(device, &viewInfo, nullptr, &resource.view), "创建 Vulkan 图像视图");
            return resource;
        } catch (...) {
            destroyImage(resource);
            throw;
        }
    }

    void immediateSubmit(VkQueue queue, VkCommandPool pool, const std::function<void(VkCommandBuffer)>& recorder,
                         VkSemaphore waitSemaphore = VK_NULL_HANDLE,
                         VkPipelineStageFlags2 waitStage = VK_PIPELINE_STAGE_2_NONE,
                         VkSemaphore signalSemaphore = VK_NULL_HANDLE,
                         VkPipelineStageFlags2 signalStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) {
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        checkVk(vkAllocateCommandBuffers(device, &allocation, &commandBuffer), "分配临时命令缓冲");

        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        try {
            checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "开始临时命令缓冲");
            recorder(commandBuffer);
            checkVk(vkEndCommandBuffer(commandBuffer), "结束临时命令缓冲");

            VkCommandBufferSubmitInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            commandInfo.commandBuffer = commandBuffer;
            VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            waitInfo.semaphore = waitSemaphore;
            waitInfo.stageMask = waitStage;
            VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            signalInfo.semaphore = signalSemaphore;
            signalInfo.stageMask = signalStage;
            VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            submitInfo.waitSemaphoreInfoCount = waitSemaphore != VK_NULL_HANDLE ? 1U : 0U;
            submitInfo.pWaitSemaphoreInfos = waitSemaphore != VK_NULL_HANDLE ? &waitInfo : nullptr;
            submitInfo.commandBufferInfoCount = 1;
            submitInfo.pCommandBufferInfos = &commandInfo;
            submitInfo.signalSemaphoreInfoCount = signalSemaphore != VK_NULL_HANDLE ? 1U : 0U;
            submitInfo.pSignalSemaphoreInfos = signalSemaphore != VK_NULL_HANDLE ? &signalInfo : nullptr;
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            VkFence fence = VK_NULL_HANDLE;
            checkVk(vkCreateFence(device, &fenceInfo, nullptr, &fence), "创建临时提交 Fence");
            const VkResult submitResult = vkQueueSubmit2(queue, 1, &submitInfo, fence);
            if (submitResult != VK_SUCCESS) {
                vkDestroyFence(device, fence, nullptr);
                fail("提交临时命令失败，VkResult=" + vkResultText(submitResult));
            }
            const VkResult waitResult = vkWaitForFences(device, 1, &fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
            vkDestroyFence(device, fence, nullptr);
            checkVk(waitResult, "等待临时提交完成");
            vkFreeCommandBuffers(device, pool, 1, &commandBuffer);
        } catch (...) {
            vkFreeCommandBuffers(device, pool, 1, &commandBuffer);
            throw;
        }
    }

    void immediateSubmit(const std::function<void(VkCommandBuffer)>& recorder) {
        immediateSubmit(graphicsQueue, commandPool, recorder);
    }

    void transitionImage(ImageResource& image, VkImageLayout newLayout, VkPipelineStageFlags2 sourceStage,
                         VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                         VkAccessFlags2 destinationAccess) {
        const VkImageLayout oldLayout = image.layout;
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = sourceStage;
        barrier.srcAccessMask = sourceAccess;
        barrier.dstStageMask = destinationStage;
        barrier.dstAccessMask = destinationAccess;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.image = image.image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        immediateSubmit([&](VkCommandBuffer commandBuffer) {
            vkCmdPipelineBarrier2(commandBuffer, &dependency);
        });
        image.layout = newLayout;
    }

    ImageResource uploadTexture(VkFormat format, const std::vector<uint8_t>& bytes) {
        return useHostImageCopy ? uploadTextureWithHostCopy(format, bytes) : uploadTextureWithTransferQueue(format, bytes);
    }

    ImageResource uploadTextureWithHostCopy(VkFormat format, const std::vector<uint8_t>& bytes) {
        if ((static_cast<uint64_t>(config.textureSize) * config.textureSize * 4) != bytes.size()) {
            fail("程序化纹理数据尺寸与 textureSize 不一致");
        }
        VkFormatProperties3 formatProperties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 formatProperties2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        formatProperties2.pNext = &formatProperties;
        vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, &formatProperties2);
        if ((formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT) == 0) {
            fail("物理设备不支持所需纹理格式的 Host Image Copy：" + std::to_string(static_cast<int>(format)));
        }

        ImageResource image = createImage(
            format,
            {config.textureSize, config.textureSize, 1},
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        try {
            transitionImage(image, hostCopyLayout, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT);

            VkMemoryToImageCopy region{VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY};
            region.pHostPointer = bytes.data();
            region.memoryRowLength = 0;
            region.memoryImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {config.textureSize, config.textureSize, 1};
            VkCopyMemoryToImageInfo copyInfo{VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO};
            copyInfo.dstImage = image.image;
            copyInfo.dstImageLayout = hostCopyLayout;
            copyInfo.regionCount = 1;
            copyInfo.pRegions = &region;
            checkVk(vkCopyMemoryToImage(device, &copyInfo), "通过 vkCopyMemoryToImage 上传程序化纹理");

            transitionImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_HOST_BIT,
                            VK_ACCESS_2_HOST_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            return image;
        } catch (...) {
            destroyImage(image);
            throw;
        }
    }

    ImageResource uploadTextureWithTransferQueue(const std::vector<uint8_t>& bytes, VkFormat format) {
        if ((static_cast<uint64_t>(config.textureSize) * config.textureSize * 4) != bytes.size()) {
            fail("程序化纹理数据尺寸与 textureSize 不一致");
        }

        ImageResource image = createImage(
            format,
            {config.textureSize, config.textureSize, 1},
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            true);
        BufferResource staging{};
        VkSemaphore transferFinished = VK_NULL_HANDLE;
        try {
            staging = createHostBuffer(bytes.data(), bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            checkVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &transferFinished), "创建传输完成信号量");
            immediateSubmit(transferQueue, transferCommandPool, [&](VkCommandBuffer commandBuffer) {
                VkImageMemoryBarrier2 toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                toTransfer.srcAccessMask = VK_ACCESS_2_NONE;
                toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toTransfer.image = image.image;
                toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                toTransfer.subresourceRange.levelCount = 1;
                toTransfer.subresourceRange.layerCount = 1;
                VkDependencyInfo toTransferDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                toTransferDependency.imageMemoryBarrierCount = 1;
                toTransferDependency.pImageMemoryBarriers = &toTransfer;
                vkCmdPipelineBarrier2(commandBuffer, &toTransferDependency);

                VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
                region.bufferOffset = 0;
                region.bufferRowLength = 0;
                region.bufferImageHeight = 0;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageOffset = {0, 0, 0};
                region.imageExtent = {config.textureSize, config.textureSize, 1};
                VkCopyBufferToImageInfo2 copyInfo{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
                copyInfo.srcBuffer = staging.buffer;
                copyInfo.dstImage = image.image;
                copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                copyInfo.regionCount = 1;
                copyInfo.pRegions = &region;
                vkCmdCopyBufferToImage2(commandBuffer, &copyInfo);

                VkImageMemoryBarrier2 toShader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                toShader.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                toShader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                toShader.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                toShader.dstAccessMask = VK_ACCESS_2_NONE;
                toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toShader.image = image.image;
                toShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                toShader.subresourceRange.levelCount = 1;
                toShader.subresourceRange.layerCount = 1;
                VkDependencyInfo toShaderDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                toShaderDependency.imageMemoryBarrierCount = 1;
                toShaderDependency.pImageMemoryBarriers = &toShader;
                vkCmdPipelineBarrier2(commandBuffer, &toShaderDependency);
            }, VK_NULL_HANDLE, VK_PIPELINE_STAGE_2_NONE, transferFinished, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);

            immediateSubmit(graphicsQueue, commandPool, [&](VkCommandBuffer commandBuffer) {
                VkImageMemoryBarrier2 readyForSampling{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                readyForSampling.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                readyForSampling.srcAccessMask = VK_ACCESS_2_NONE;
                readyForSampling.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                readyForSampling.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                readyForSampling.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                readyForSampling.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                readyForSampling.image = image.image;
                readyForSampling.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                readyForSampling.subresourceRange.levelCount = 1;
                readyForSampling.subresourceRange.layerCount = 1;
                VkDependencyInfo readyDependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                readyDependency.imageMemoryBarrierCount = 1;
                readyDependency.pImageMemoryBarriers = &readyForSampling;
                vkCmdPipelineBarrier2(commandBuffer, &readyDependency);
            }, transferFinished, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
            vkDestroySemaphore(device, transferFinished, nullptr);
            transferFinished = VK_NULL_HANDLE;
            destroyBuffer(staging);
            image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return image;
        } catch (...) {
            if (transferFinished != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, transferFinished, nullptr);
            }
            destroyBuffer(staging);
            destroyImage(image);
            throw;
        }
    }

    ImageResource uploadTextureWithTransferQueue(VkFormat format, const std::vector<uint8_t>& bytes) {
        return uploadTextureWithTransferQueue(bytes, format);
    }

    void createTextures() {
        materials.reserve(config.materials.size());
        for (const MaterialConfig& material : config.materials) {
            materials.push_back({});
            RuntimeMaterial& runtime = materials.back();
            runtime.config = material;
            const GeneratedTextures generated = generateTextures(material, config.textureSize);
            runtime.baseColor = uploadTexture(VK_FORMAT_R8G8B8A8_SRGB, generated.baseColor);
            runtime.roughness = uploadTexture(VK_FORMAT_R8G8B8A8_UNORM, generated.roughness);
            runtime.normal = uploadTexture(VK_FORMAT_R8G8B8A8_UNORM, generated.normal);
        }
    }


    void createDescriptorResources() {
        VkPhysicalDevicePushDescriptorPropertiesKHR pushProperties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &pushProperties;
        vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
        if (pushProperties.maxPushDescriptors < 3) {
            fail("物理设备的 Push Descriptor 容量不足以绑定三张材质纹理");
        }

        const std::array<VkDescriptorSetLayoutBinding, 3> bindings = {{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        checkVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout), "创建 Push Descriptor 布局");

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(PushConstants);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        checkVk(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout), "创建管线布局");

        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxLod = 0.0f;
        checkVk(vkCreateSampler(device, &samplerInfo, nullptr, &sampler), "创建材质采样器");
    }

    void createGeometry() {
        vertices = makeCubeVertices(indices);
        vertexBuffer = createHostBuffer(vertices.data(), sizeof(Vertex) * vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        indexBuffer = createHostBuffer(indices.data(), sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    }

    VkFormat chooseDepthFormat() const {
        const std::array<VkFormat, 3> candidates = {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
        };
        for (VkFormat candidate : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &properties);
            if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
                return candidate;
            }
        }
        fail("物理设备不支持可用的深度附件格式");
    }

    VkSurfaceFormatKHR chooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) const {
        for (const VkSurfaceFormatKHR& format : formats) {
            if ((format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_SRGB) &&
                format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
        fail("Surface 不支持所需的 sRGB 交换链格式");
    }

    VkPresentModeKHR choosePresentMode(const std::vector<VkPresentModeKHR>& modes) const {
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR) == modes.end()) {
            fail("Surface 不支持 FIFO 呈现模式");
        }
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseSwapchainExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        }
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        VkExtent2D extent{
            static_cast<uint32_t>(std::max(width, 1)),
            static_cast<uint32_t>(std::max(height, 1)),
        };
        extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        return extent;
    }

    void createDepthResources() {
        depthFormat = chooseDepthFormat();
        depthImage = createImage(
            depthFormat,
            {swapchainExtent.width, swapchainExtent.height, 1},
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
        depthNeedsTransition = true;
    }

    void createSwapchain() {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while ((width == 0 || height == 0) && !glfwWindowShouldClose(window)) {
            glfwWaitEvents();
            glfwGetFramebufferSize(window, &width, &height);
        }
        if (glfwWindowShouldClose(window)) {
            return;
        }

        VkSurfaceCapabilitiesKHR capabilities{};
        checkVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities), "查询 Surface 能力");
        uint32_t formatCount = 0;
        checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr), "查询 Surface 格式数量");
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, formats.data()), "读取 Surface 格式");
        uint32_t presentModeCount = 0;
        checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr), "查询呈现模式数量");
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, presentModes.data()), "读取呈现模式");

        const VkSurfaceFormatKHR surfaceFormat = chooseSurfaceFormat(formats);
        const VkPresentModeKHR presentMode = choosePresentMode(presentModes);
        const VkExtent2D extent = chooseSwapchainExtent(capabilities);
        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
            imageCount = capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR swapchainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchainInfo.surface = surface;
        swapchainInfo.minImageCount = imageCount;
        swapchainInfo.imageFormat = surfaceFormat.format;
        swapchainInfo.imageColorSpace = surfaceFormat.colorSpace;
        swapchainInfo.imageExtent = extent;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.preTransform = capabilities.currentTransform;
        swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        swapchainInfo.presentMode = presentMode;
        swapchainInfo.clipped = VK_TRUE;
        checkVk(vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain), "创建交换链");
        swapchainFormat = surfaceFormat.format;
        swapchainExtent = extent;

        uint32_t actualImageCount = 0;
        checkVk(vkGetSwapchainImagesKHR(device, swapchain, &actualImageCount, nullptr), "查询交换链图像数量");
        swapchainImages.resize(actualImageCount);
        checkVk(vkGetSwapchainImagesKHR(device, swapchain, &actualImageCount, swapchainImages.data()), "读取交换链图像");
        swapchainImageViews.resize(swapchainImages.size());
        for (size_t index = 0; index < swapchainImages.size(); ++index) {
            VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            viewInfo.image = swapchainImages[index];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = swapchainFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            checkVk(vkCreateImageView(device, &viewInfo, nullptr, &swapchainImageViews[index]), "创建交换链图像视图");
        }
        swapchainImageLayouts.assign(swapchainImages.size(), VK_IMAGE_LAYOUT_UNDEFINED);
        imagesInFlight.assign(swapchainImages.size(), VK_NULL_HANDLE);
        createDepthResources();
    }

    void destroySwapchain() {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        destroyImage(depthImage);
        depthFormat = VK_FORMAT_UNDEFINED;
        for (VkImageView view : swapchainImageViews) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device, view, nullptr);
            }
        }
        swapchainImageViews.clear();
        swapchainImages.clear();
        swapchainImageLayouts.clear();
        imagesInFlight.clear();
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            swapchain = VK_NULL_HANDLE;
        }
    }

    void recreateSwapchain() {
        checkVk(vkDeviceWaitIdle(device), "等待交换链重建前设备空闲");
        if (graphicsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, graphicsPipeline, nullptr);
            graphicsPipeline = VK_NULL_HANDLE;
        }
        destroySwapchain();
        createSwapchain();
        if (swapchain != VK_NULL_HANDLE) {
            createGraphicsPipeline();
        }
        framebufferResized = false;
    }

    void createGraphicsPipeline() {
        VkShaderModuleCreateInfo vertexModuleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        vertexModuleInfo.codeSize = sizeof(kVertexShaderCode);
        vertexModuleInfo.pCode = kVertexShaderCode;
        VkShaderModule vertexModule = VK_NULL_HANDLE;
        checkVk(vkCreateShaderModule(device, &vertexModuleInfo, nullptr, &vertexModule), "创建顶点 Shader 模块");
        VkShaderModuleCreateInfo fragmentModuleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        fragmentModuleInfo.codeSize = sizeof(kFragmentShaderCode);
        fragmentModuleInfo.pCode = kFragmentShaderCode;
        VkShaderModule fragmentModule = VK_NULL_HANDLE;
        try {
            checkVk(vkCreateShaderModule(device, &fragmentModuleInfo, nullptr, &fragmentModule), "创建片段 Shader 模块");

            const std::array<VkPipelineShaderStageCreateInfo, 2> stages = {{
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexModule, "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule, "main", nullptr},
            }};
            const std::array<VkVertexInputBindingDescription, 1> bindings = {{
                {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
            }};
            const std::array<VkVertexInputAttributeDescription, 4> attributes = {{
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
                {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, tangent)},
                {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
            }};
            VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
            vertexInput.pVertexBindingDescriptions = bindings.data();
            vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
            vertexInput.pVertexAttributeDescriptions = attributes.data();

            VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport.viewportCount = 1;
            viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rasterization.polygonMode = VK_POLYGON_MODE_FILL;
            rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
            rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rasterization.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            colorBlend.attachmentCount = 1;
            colorBlend.pAttachments = &colorBlendAttachment;
            const std::array<VkDynamicState, 2> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamicState.pDynamicStates = dynamicStates.data();

            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &swapchainFormat;
            rendering.depthAttachmentFormat = depthFormat;
            VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            pipelineInfo.pNext = &rendering;
            pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
            pipelineInfo.pStages = stages.data();
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewport;
            pipelineInfo.pRasterizationState = &rasterization;
            pipelineInfo.pMultisampleState = &multisample;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlend;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = pipelineLayout;
            checkVk(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline), "创建动态渲染管线");
            vkDestroyShaderModule(device, fragmentModule, nullptr);
            vkDestroyShaderModule(device, vertexModule, nullptr);
        } catch (...) {
            if (fragmentModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device, fragmentModule, nullptr);
            }
            if (vertexModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device, vertexModule, nullptr);
            }
            throw;
        }
    }

    void createFrameResources() {
        std::array<VkCommandBuffer, kFramesInFlight> commandBuffers{};
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = kFramesInFlight;
        checkVk(vkAllocateCommandBuffers(device, &allocation, commandBuffers.data()), "分配帧命令缓冲");
        for (uint32_t index = 0; index < kFramesInFlight; ++index) {
            frames[index].commandBuffer = commandBuffers[index];
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            checkVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frames[index].imageAvailable), "创建图像可用信号量");
            checkVk(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &frames[index].renderFinished), "创建渲染完成信号量");
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            checkVk(vkCreateFence(device, &fenceInfo, nullptr, &frames[index].inFlight), "创建帧 Fence");
        }
    }

    void buildScene() {
        const uint32_t metalIndex = config.indexById.at("brushed_metal");
        const uint32_t redIndex = config.indexById.at("red_plastic");
        scene.clear();
        scene.reserve(99);
        for (int z = 0; z < 3; ++z) {
            for (int x = 0; x < 3; ++x) {
                scene.push_back({{static_cast<float>(x - 1), 0.5f, static_cast<float>(z - 1)}, metalIndex});
            }
        }
        const std::array<std::array<int, 2>, 6> middleCells = {{{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {2, 0}}};
        for (const auto& cell : middleCells) {
            scene.push_back({{static_cast<float>(cell[0] - 1), 1.5f, static_cast<float>(cell[1] - 1)}, redIndex});
        }
        const std::array<std::array<int, 2>, 3> topCells = {{{0, 0}, {0, 1}, {1, 2}}};
        for (const auto& cell : topCells) {
            scene.push_back({{static_cast<float>(cell[0] - 1), 2.5f, static_cast<float>(cell[1] - 1)}, redIndex});
        }
        for (int z = 0; z < 9; ++z) {
            for (int x = 0; x < 9; ++x) {
                scene.push_back({{static_cast<float>(x - 4), -0.5f, static_cast<float>(z - 4)}, metalIndex});
            }
        }
    }

    void recordImageBarrier(VkCommandBuffer commandBuffer, const std::vector<VkImageMemoryBarrier2>& barriers) const {
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
    }

    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
        VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "开始帧命令缓冲");

        std::vector<VkImageMemoryBarrier2> preRenderBarriers;
        preRenderBarriers.reserve(depthNeedsTransition ? 2 : 1);
        VkImageMemoryBarrier2 colorBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        colorBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        colorBarrier.srcAccessMask = VK_ACCESS_2_NONE;
        colorBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        colorBarrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        colorBarrier.oldLayout = swapchainImageLayouts[imageIndex];
        colorBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorBarrier.image = swapchainImages[imageIndex];
        colorBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        colorBarrier.subresourceRange.levelCount = 1;
        colorBarrier.subresourceRange.layerCount = 1;
        preRenderBarriers.push_back(colorBarrier);

        if (depthNeedsTransition) {
            VkImageMemoryBarrier2 depthBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            depthBarrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            depthBarrier.srcAccessMask = VK_ACCESS_2_NONE;
            depthBarrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            depthBarrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            depthBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthBarrier.image = depthImage.image;
            depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            depthBarrier.subresourceRange.levelCount = 1;
            depthBarrier.subresourceRange.layerCount = 1;
            preRenderBarriers.push_back(depthBarrier);
            depthNeedsTransition = false;
        }
        recordImageBarrier(commandBuffer, preRenderBarriers);
        swapchainImageLayouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkClearValue colorClear{};
        colorClear.color = {{0.018f, 0.022f, 0.030f, 1.0f}};
        VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAttachment.imageView = swapchainImageViews[imageIndex];
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = colorClear;
        VkClearValue depthClear{};
        depthClear.depthStencil = {1.0f, 0};
        VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depthAttachment.imageView = depthImage.view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue = depthClear;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = swapchainExtent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &colorAttachment;
        rendering.pDepthAttachment = &depthAttachment;
        vkCmdBeginRendering(commandBuffer, &rendering);

        VkViewport viewport{};
        viewport.width = static_cast<float>(swapchainExtent.width);
        viewport.height = static_cast<float>(swapchainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{{0, 0}, swapchainExtent};
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        const VkBuffer vertexBuffers[] = {vertexBuffer.buffer};
        const VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        const Vec3 camera = {5.0f, 4.0f, 6.0f};
        const Mat4 view = lookAtMatrix(camera, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
        const float aspect = static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height);
        const Mat4 projection = perspectiveMatrix(60.0f * kPi / 180.0f, aspect, 0.1f, 100.0f);
        const Mat4 viewProjection = multiply(projection, view);
        const Vec3 lightReference = {4.5f, 6.0f, 4.5f};
        const Vec3 lightDirection = normalize(Vec3{0.0f, 1.5f, 0.0f} - lightReference);

        for (const SceneObject& object : scene) {
            const RuntimeMaterial& material = materials[object.materialIndex];
            const Mat4 model = translationMatrix(object.position);
            const PushConstants pushConstants{
                viewProjection,
                model,
                {material.config.metallic, material.config.roughness, material.config.ambientOcclusion, material.config.normalStrength},
                {camera.x, camera.y, camera.z, 1.0f},
                {lightDirection.x, lightDirection.y, lightDirection.z, 1.0f},
            };
            const std::array<VkDescriptorImageInfo, 3> imageInfos = {{
                {sampler, material.baseColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {sampler, material.roughness.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {sampler, material.normal.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            }};
            std::array<VkWriteDescriptorSet, 3> writes{};
            for (uint32_t binding = 0; binding < writes.size(); ++binding) {
                writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[binding].dstSet = VK_NULL_HANDLE;
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
                writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[binding].pImageInfo = &imageInfos[binding];
            }
            vkCmdPushDescriptorSetKHR(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                                      static_cast<uint32_t>(writes.size()), writes.data());
            vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pushConstants), &pushConstants);
            vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(indices.size()), 1, 0, 0, 0);
        }
        vkCmdEndRendering(commandBuffer);

        VkImageMemoryBarrier2 presentBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        presentBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        presentBarrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        presentBarrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        presentBarrier.dstAccessMask = VK_ACCESS_2_NONE;
        presentBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        presentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        presentBarrier.image = swapchainImages[imageIndex];
        presentBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        presentBarrier.subresourceRange.levelCount = 1;
        presentBarrier.subresourceRange.layerCount = 1;
        recordImageBarrier(commandBuffer, {presentBarrier});
        swapchainImageLayouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        checkVk(vkEndCommandBuffer(commandBuffer), "结束帧命令缓冲");
    }

    void drawFrame() {
        if (swapchain == VK_NULL_HANDLE) {
            return;
        }
        FrameResources& frame = frames[currentFrame];
        checkVk(vkWaitForFences(device, 1, &frame.inFlight, VK_TRUE, std::numeric_limits<uint64_t>::max()), "等待帧 Fence");

        uint32_t imageIndex = 0;
        const VkResult acquireResult = vkAcquireNextImageKHR(
            device, swapchain, std::numeric_limits<uint64_t>::max(), frame.imageAvailable, VK_NULL_HANDLE, &imageIndex);
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
            return;
        }
        if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
            checkVk(acquireResult, "获取交换链图像");
        }

        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            checkVk(vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, std::numeric_limits<uint64_t>::max()), "等待交换链图像 Fence");
        }
        imagesInFlight[imageIndex] = frame.inFlight;
        checkVk(vkResetFences(device, 1, &frame.inFlight), "重置帧 Fence");
        checkVk(vkResetCommandBuffer(frame.commandBuffer, 0), "重置帧命令缓冲");
        recordCommandBuffer(frame.commandBuffer, imageIndex);

        VkSemaphoreSubmitInfo waitSemaphore{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waitSemaphore.semaphore = frame.imageAvailable;
        waitSemaphore.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkCommandBufferSubmitInfo commandBufferInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commandBufferInfo.commandBuffer = frame.commandBuffer;
        VkSemaphoreSubmitInfo signalSemaphore{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signalSemaphore.semaphore = frame.renderFinished;
        signalSemaphore.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
        VkSubmitInfo2 submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submitInfo.waitSemaphoreInfoCount = 1;
        submitInfo.pWaitSemaphoreInfos = &waitSemaphore;
        submitInfo.commandBufferInfoCount = 1;
        submitInfo.pCommandBufferInfos = &commandBufferInfo;
        submitInfo.signalSemaphoreInfoCount = 1;
        submitInfo.pSignalSemaphoreInfos = &signalSemaphore;
        checkVk(vkQueueSubmit2(graphicsQueue, 1, &submitInfo, frame.inFlight), "提交帧命令");

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &frame.renderFinished;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &imageIndex;
        const VkResult presentResult = vkQueuePresentKHR(graphicsQueue, &present);
        if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR && presentResult != VK_ERROR_OUT_OF_DATE_KHR) {
            checkVk(presentResult, "呈现交换链图像");
        }
        if (framebufferResized || acquireResult == VK_SUBOPTIMAL_KHR || presentResult == VK_SUBOPTIMAL_KHR ||
            presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchain();
        }
        currentFrame = (currentFrame + 1) % kFramesInFlight;
    }

    void cleanup() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            if (graphicsPipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device, graphicsPipeline, nullptr);
                graphicsPipeline = VK_NULL_HANDLE;
            }
            destroySwapchain();
            for (RuntimeMaterial& material : materials) {
                destroyImage(material.baseColor);
                destroyImage(material.roughness);
                destroyImage(material.normal);
            }
            materials.clear();
            if (sampler != VK_NULL_HANDLE) {
                vkDestroySampler(device, sampler, nullptr);
                sampler = VK_NULL_HANDLE;
            }
            if (pipelineLayout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
                pipelineLayout = VK_NULL_HANDLE;
            }
            if (descriptorSetLayout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
                descriptorSetLayout = VK_NULL_HANDLE;
            }
            destroyBuffer(indexBuffer);
            destroyBuffer(vertexBuffer);
            for (FrameResources& frame : frames) {
                if (frame.imageAvailable != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, frame.imageAvailable, nullptr);
                }
                if (frame.renderFinished != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device, frame.renderFinished, nullptr);
                }
                if (frame.inFlight != VK_NULL_HANDLE) {
                    vkDestroyFence(device, frame.inFlight, nullptr);
                }
                frame = {};
            }
            if (transferCommandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, transferCommandPool, nullptr);
                transferCommandPool = VK_NULL_HANDLE;
            }
            if (commandPool != VK_NULL_HANDLE) {
                vkDestroyCommandPool(device, commandPool, nullptr);
                commandPool = VK_NULL_HANDLE;
            }
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
        if (volkInitialized) {
            volkFinalize();
            volkInitialized = false;
        }
        if (window != nullptr) {
            glfwDestroyWindow(window);
            window = nullptr;
        }
        if (glfwInitialized) {
            glfwTerminate();
            glfwInitialized = false;
        }
    }
};

} // 命名空间

int main() {
    try {
        RubikCubeApp application;
        application.run();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "RubikCube 错误：" << exception.what() << '\n';
        return 1;
    }
}
