#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
constexpr int WindowWidth = 1280;
constexpr int WindowHeight = 720;
constexpr int SceneWidth = 640;
constexpr int SceneHeight = 360;
constexpr int TextureSize = 32;
constexpr int MaterialCount = 2;
constexpr int GroundCount = 225;
constexpr int PillarCount = 25;
constexpr int TotalInstanceCount = GroundCount + PillarCount;
constexpr int ShadowMapSize = 2048;
constexpr int EnvironmentSize = 256;
constexpr int IrradianceSize = 32;
constexpr int PrefilterSize = 128;
constexpr int PrefilterMipCount = 5;
constexpr int BrdfLutSize = 256;

struct MaterialDefinition
{
    std::string id;
    glm::vec3 baseColorSrgb{};
    float metallic = 0.0f;
    float roughness = 0.0f;
    float ambientOcclusion = 0.0f;
    float normalStrength = 0.0f;
    float baseColorVariation = 0.0f;
    float roughnessVariation = 0.0f;
    std::uint32_t textureSeed = 0;
    std::string pattern;
    std::uint32_t layer = 0;
};

struct InstanceData
{
    glm::mat4 model{ 1.0f };
    std::uint32_t materialIndex = 0;
};

struct SceneInstance
{
    InstanceData gpu;
    glm::vec3 aabbCenter{};
};

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct Plane
{
    glm::vec3 normal{};
    float distance = 0.0f;
};

struct FrameGpu
{
    glm::mat4 view{ 1.0f };
    glm::mat4 projection{ 1.0f };
    glm::mat4 lightSpace{ 1.0f };
    glm::vec4 cameraPosition{};
    glm::vec4 sunDirection{};
    glm::vec4 sunRadiance{};
    std::array<glm::vec4, 4> pointPositions{};
    std::array<glm::vec4, 4> pointRadiances{};
    glm::ivec4 settings{};
};

static_assert(sizeof(glm::mat4) == sizeof(float) * 16);
static_assert(sizeof(FrameGpu) == 384);

class Camera
{
public:
    glm::vec3 position{ 18.0f, 14.0f, 18.0f };

    void LookAt(const glm::vec3& target)
    {
        const glm::vec3 direction = glm::normalize(target - position);
        yawDegrees_ = glm::degrees(std::atan2(direction.z, direction.x));
        pitchDegrees_ = glm::degrees(std::asin(direction.y));
        UpdateVectors();
    }

    void Move(float forwardAmount, float rightAmount)
    {
        position += front_ * forwardAmount + right_ * rightAmount;
    }

    void Rotate(float yawOffset, float pitchOffset)
    {
        yawDegrees_ += yawOffset;
        pitchDegrees_ = std::clamp(pitchDegrees_ + pitchOffset, -89.0f, 89.0f);
        UpdateVectors();
    }

    [[nodiscard]] glm::mat4 ViewMatrix() const
    {
        return glm::lookAt(position, position + front_, up_);
    }

private:
    void UpdateVectors()
    {
        const float yawRadians = glm::radians(yawDegrees_);
        const float pitchRadians = glm::radians(pitchDegrees_);
        front_ = glm::normalize(glm::vec3(
            std::cos(yawRadians) * std::cos(pitchRadians),
            std::sin(pitchRadians),
            std::sin(yawRadians) * std::cos(pitchRadians)));
        right_ = glm::normalize(glm::cross(front_, worldUp_));
        up_ = glm::normalize(glm::cross(right_, front_));
    }

    glm::vec3 front_{ 0.0f, 0.0f, -1.0f };
    glm::vec3 right_{ 1.0f, 0.0f, 0.0f };
    glm::vec3 up_{ 0.0f, 1.0f, 0.0f };
    const glm::vec3 worldUp_{ 0.0f, 1.0f, 0.0f };
    float yawDegrees_ = -135.0f;
    float pitchDegrees_ = -25.0f;
};

float HashNoise(const std::uint32_t seed, const int x, const int y)
{
    std::uint32_t value = seed;
    value ^= static_cast<std::uint32_t>(x) * 0x9e3779b9u;
    value ^= static_cast<std::uint32_t>(y) * 0x85ebca6bu;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return static_cast<float>(value & 0x00ffffffu) / static_cast<float>(0x01000000u);
}

float SignedNoise(const std::uint32_t seed, const int x, const int y)
{
    return HashNoise(seed, x, y) * 2.0f - 1.0f;
}

void PrintFieldError(const fs::path& source, const std::string& field, const std::string& message)
{
    std::cerr << source.string() << ": 字段 '" << field << "' " << message << '\n';
}

bool ReadStringField(const json& object, const std::string& key, std::string& destination,
    const fs::path& source, const std::string& field)
{
    if (!object.contains(key))
    {
        PrintFieldError(source, field, "缺失。");
        return false;
    }
    const json& value = object.at(key);
    if (!value.is_string())
    {
        PrintFieldError(source, field, "必须是字符串。");
        return false;
    }
    destination = value.get<std::string>();
    return true;
}

bool ReadFloatField(const json& object, const std::string& key, float& destination,
    const fs::path& source, const std::string& field)
{
    if (!object.contains(key))
    {
        PrintFieldError(source, field, "缺失。");
        return false;
    }
    const json& value = object.at(key);
    if (!value.is_number())
    {
        PrintFieldError(source, field, "必须是数值。");
        return false;
    }
    const double parsed = value.get<double>();
    if (!std::isfinite(parsed) || parsed < -std::numeric_limits<float>::max()
        || parsed > std::numeric_limits<float>::max())
    {
        PrintFieldError(source, field, "必须是有限的 float 数值。");
        return false;
    }
    destination = static_cast<float>(parsed);
    return true;
}

bool ReadUnsignedField(const json& object, const std::string& key, std::uint32_t& destination,
    const fs::path& source, const std::string& field)
{
    if (!object.contains(key))
    {
        PrintFieldError(source, field, "缺失。");
        return false;
    }
    const json& value = object.at(key);
    if ((!value.is_number_integer() && !value.is_number_unsigned()))
    {
        PrintFieldError(source, field, "必须是非负整数。");
        return false;
    }
    try
    {
        const std::int64_t parsed = value.get<std::int64_t>();
        if (parsed < 0 || static_cast<std::uint64_t>(parsed) > std::numeric_limits<std::uint32_t>::max())
        {
            PrintFieldError(source, field, "超出 uint32 范围。");
            return false;
        }
        destination = static_cast<std::uint32_t>(parsed);
        return true;
    }
    catch (const json::exception& error)
    {
        PrintFieldError(source, field, std::string("无法读取：") + error.what());
        return false;
    }
}

bool ReadColorField(const json& object, const std::string& key, glm::vec3& destination,
    const fs::path& source, const std::string& field)
{
    if (!object.contains(key))
    {
        PrintFieldError(source, field, "缺失。");
        return false;
    }
    const json& value = object.at(key);
    if (!value.is_array() || value.size() != 3)
    {
        PrintFieldError(source, field, "必须是恰好含 3 个数值的数组。");
        return false;
    }
    for (std::size_t component = 0; component < 3; ++component)
    {
        if (!value.at(component).is_number())
        {
            PrintFieldError(source, field + "[" + std::to_string(component) + "]", "必须是数值。");
            return false;
        }
        const double parsed = value.at(component).get<double>();
        if (!std::isfinite(parsed) || parsed < 0.0 || parsed > 1.0)
        {
            PrintFieldError(source, field + "[" + std::to_string(component) + "]", "必须位于 [0, 1]。");
            return false;
        }
        destination[static_cast<glm::length_t>(component)] = static_cast<float>(parsed);
    }
    return true;
}

bool IsUnitInterval(const float value)
{
    return value >= 0.0f && value <= 1.0f && std::isfinite(value);
}

class Application
{
public:
    bool Run()
    {
        if (!Initialize())
        {
            Shutdown();
            return false;
        }

        double previousTime = glfwGetTime();
        double titleTime = previousTime;
        std::uint32_t framesSinceTitle = 0;

        while (glfwWindowShouldClose(window_) == GLFW_FALSE)
        {
            glfwPollEvents();
            const double currentTime = glfwGetTime();
            const float deltaTime = static_cast<float>(std::min(currentTime - previousTime, 0.1));
            previousTime = currentTime;

            ProcessMovement(deltaTime);
            RenderFrame();
            glfwSwapBuffers(window_);

            ++framesSinceTitle;
            if (currentTime - titleTime >= 1.0)
            {
                UpdateWindowTitle(static_cast<double>(framesSinceTitle) / (currentTime - titleTime));
                titleTime = currentTime;
                framesSinceTitle = 0;
            }
        }

        Shutdown();
        return true;
    }

private:
    bool Initialize()
    {
        if (!ResolveProjectDirectory() || !LoadMaterials())
        {
            return false;
        }

        if (glfwInit() == GLFW_FALSE)
        {
            std::cerr << "GLFW 初始化失败。\n";
            return false;
        }
        glfwInitialized_ = true;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_SAMPLES, 0);

        window_ = glfwCreateWindow(WindowWidth, WindowHeight,
            "OpenGL VoxelPBRFrustumCulling Test", nullptr, nullptr);
        if (window_ == nullptr)
        {
            std::cerr << "无法创建 OpenGL 4.6 core profile GLFW 窗口。\n";
            return false;
        }

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);
        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0)
        {
            std::cerr << "GLAD 无法加载 OpenGL 函数。\n";
            return false;
        }
        glReady_ = true;

        GLint majorVersion = 0;
        GLint minorVersion = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
        glGetIntegerv(GL_MINOR_VERSION, &minorVersion);
        if (majorVersion < 4 || (majorVersion == 4 && minorVersion < 6))
        {
            std::cerr << "需要 OpenGL 4.6，当前上下文为 " << majorVersion << '.' << minorVersion << "。\n";
            return false;
        }

        std::cout << "GPU Vendor: " << glGetString(GL_VENDOR) << '\n';
        std::cout << "GPU Renderer: " << glGetString(GL_RENDERER) << '\n';
        std::cout << "OpenGL Version: " << glGetString(GL_VERSION) << '\n';
        std::cout << "GLSL Version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << '\n';

        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, FramebufferSizeCallback);
        glfwSetCursorPosCallback(window_, CursorPositionCallback);
        glfwSetMouseButtonCallback(window_, MouseButtonCallback);
        glfwSetKeyCallback(window_, KeyCallback);
        glfwGetFramebufferSize(window_, &framebufferWidth_, &framebufferHeight_);
        SetMouseCapture(true);

        camera_.LookAt(glm::vec3(0.0f, 2.5f, 0.0f));
        BuildStaticInstances();

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glDisable(GL_MULTISAMPLE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

        if (!CreatePrograms() || !CreateGeometry() || !CreateFrameUbo() || !CreateMaterialTextures()
            || !CreateSceneFramebuffer() || !CreateShadowFramebuffer() || !CreateFullscreenVao()
            || !CreateIblTextures() || !GenerateImageBasedLighting())
        {
            return false;
        }

        ConfigureSamplerBindings();
        return true;
    }

    bool ResolveProjectDirectory()
    {
        std::error_code error;
        const fs::path directCandidate = fs::current_path(error) / "materials.json";
        if (!error && fs::is_regular_file(directCandidate, error) && !error)
        {
            projectDirectory_ = directCandidate.parent_path();
            materialsPath_ = directCandidate;
            return true;
        }

        std::array<wchar_t, 32768> executableBuffer{};
        const DWORD length = GetModuleFileNameW(nullptr, executableBuffer.data(),
            static_cast<DWORD>(executableBuffer.size()));
        if (length == 0 || length >= executableBuffer.size())
        {
            std::cerr << "无法确定可执行文件路径，无法定位 materials.json。\n";
            return false;
        }

        fs::path cursor = fs::path(executableBuffer.data()).parent_path();
        for (int depth = 0; depth < 4; ++depth)
        {
            const fs::path candidate = cursor / "materials.json";
            error.clear();
            if (fs::is_regular_file(candidate, error) && !error)
            {
                projectDirectory_ = cursor;
                materialsPath_ = candidate;
                return true;
            }
            cursor = cursor.parent_path();
        }

        std::cerr << "找不到项目目录中的 materials.json。\n";
        return false;
    }

    bool LoadMaterials()
    {
        std::ifstream input(materialsPath_, std::ios::binary);
        if (!input)
        {
            std::cerr << "无法打开材质文件：" << materialsPath_.string() << '\n';
            return false;
        }

        json root;
        try
        {
            input >> root;
        }
        catch (const json::parse_error& error)
        {
            std::cerr << materialsPath_.string() << ": JSON 语法错误：" << error.what() << '\n';
            return false;
        }
        catch (const json::exception& error)
        {
            std::cerr << materialsPath_.string() << ": 无法解析 JSON：" << error.what() << '\n';
            return false;
        }

        if (!root.is_object())
        {
            PrintFieldError(materialsPath_, "根对象", "必须是对象。");
            return false;
        }
        if (!root.contains("schemaVersion") || !root.at("schemaVersion").is_number_integer())
        {
            PrintFieldError(materialsPath_, "schemaVersion", "必须是整数且缺失时不可省略。");
            return false;
        }
        try
        {
            if (root.at("schemaVersion").get<int>() != 1)
            {
                PrintFieldError(materialsPath_, "schemaVersion", "不受支持；仅支持版本 1。");
                return false;
            }
        }
        catch (const json::exception& error)
        {
            PrintFieldError(materialsPath_, "schemaVersion", std::string("无法读取：") + error.what());
            return false;
        }
        if (!root.contains("textureSize") || !root.at("textureSize").is_number_integer())
        {
            PrintFieldError(materialsPath_, "textureSize", "必须是整数且缺失时不可省略。");
            return false;
        }
        try
        {
            if (root.at("textureSize").get<int>() != TextureSize)
            {
                PrintFieldError(materialsPath_, "textureSize", "必须严格为 32。");
                return false;
            }
        }
        catch (const json::exception& error)
        {
            PrintFieldError(materialsPath_, "textureSize", std::string("无法读取：") + error.what());
            return false;
        }
        if (!root.contains("materials") || !root.at("materials").is_array())
        {
            PrintFieldError(materialsPath_, "materials", "必须是数组。");
            return false;
        }
        const json& materialArray = root.at("materials");
        if (materialArray.size() != MaterialCount)
        {
            PrintFieldError(materialsPath_, "materials", "必须恰好包含两种材质。");
            return false;
        }

        std::array<bool, MaterialCount> seen{};
        for (std::size_t index = 0; index < materialArray.size(); ++index)
        {
            const json& entry = materialArray.at(index);
            const std::string prefix = "materials[" + std::to_string(index) + "].";
            if (!entry.is_object())
            {
                PrintFieldError(materialsPath_, "materials[" + std::to_string(index) + "]", "必须是对象。");
                return false;
            }

            MaterialDefinition definition;
            if (!ReadStringField(entry, "id", definition.id, materialsPath_, prefix + "id")
                || !ReadColorField(entry, "baseColorSRGB", definition.baseColorSrgb, materialsPath_, prefix + "baseColorSRGB")
                || !ReadFloatField(entry, "metallic", definition.metallic, materialsPath_, prefix + "metallic")
                || !ReadFloatField(entry, "roughness", definition.roughness, materialsPath_, prefix + "roughness")
                || !ReadFloatField(entry, "ambientOcclusion", definition.ambientOcclusion, materialsPath_, prefix + "ambientOcclusion")
                || !ReadFloatField(entry, "normalStrength", definition.normalStrength, materialsPath_, prefix + "normalStrength")
                || !ReadFloatField(entry, "baseColorVariation", definition.baseColorVariation, materialsPath_, prefix + "baseColorVariation")
                || !ReadFloatField(entry, "roughnessVariation", definition.roughnessVariation, materialsPath_, prefix + "roughnessVariation")
                || !ReadUnsignedField(entry, "textureSeed", definition.textureSeed, materialsPath_, prefix + "textureSeed")
                || !ReadStringField(entry, "pattern", definition.pattern, materialsPath_, prefix + "pattern"))
            {
                return false;
            }

            if (!IsUnitInterval(definition.metallic) || !IsUnitInterval(definition.roughness)
                || !IsUnitInterval(definition.ambientOcclusion) || !IsUnitInterval(definition.normalStrength)
                || !IsUnitInterval(definition.baseColorVariation) || !IsUnitInterval(definition.roughnessVariation))
            {
                PrintFieldError(materialsPath_, "materials[" + std::to_string(index) + "]",
                    "所有 PBR 数值与 variation 必须位于 [0, 1]。");
                return false;
            }

            if (definition.id == "brushed_metal")
            {
                definition.layer = 0;
                if (definition.pattern != "brushed_x")
                {
                    PrintFieldError(materialsPath_, prefix + "pattern", "brushed_metal 必须使用 brushed_x。");
                    return false;
                }
            }
            else if (definition.id == "red_plastic")
            {
                definition.layer = 1;
                if (definition.pattern != "molded")
                {
                    PrintFieldError(materialsPath_, prefix + "pattern", "red_plastic 必须使用 molded。");
                    return false;
                }
            }
            else
            {
                PrintFieldError(materialsPath_, prefix + "id", "不是受支持的固定材质标识符。");
                return false;
            }
            if (seen[definition.layer])
            {
                PrintFieldError(materialsPath_, prefix + "id", "重复定义。");
                return false;
            }
            seen[definition.layer] = true;
            materials_[definition.layer] = std::move(definition);
        }

        if (!seen[0] || !seen[1])
        {
            PrintFieldError(materialsPath_, "materials", "缺少 brushed_metal 或 red_plastic。");
            return false;
        }
        return true;
    }

    std::optional<std::string> ReadShaderText(const fs::path& relativePath) const
    {
        const fs::path filePath = projectDirectory_ / relativePath;
        std::ifstream input(filePath, std::ios::binary);
        if (!input)
        {
            std::cerr << "无法读取 shader 文件：" << filePath.string() << '\n';
            return std::nullopt;
        }
        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef
            && static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf)
        {
            text.erase(0, 3);
        }
        if (text.empty())
        {
            std::cerr << "shader 文件为空：" << filePath.string() << '\n';
            return std::nullopt;
        }
        return text;
    }

    GLuint CompileShader(const GLenum type, const std::string& source, const fs::path& path) const
    {
        const GLuint shader = glCreateShader(type);
        const char* sourcePointer = source.data();
        const GLint sourceLength = static_cast<GLint>(source.size());
        glShaderSource(shader, 1, &sourcePointer, &sourceLength);
        glCompileShader(shader);

        GLint succeeded = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &succeeded);
        if (succeeded == GL_TRUE)
        {
            return shader;
        }

        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(std::max(logLength, 1));
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::cerr << "shader 编译失败（" << path.string() << "）：\n" << log.data() << '\n';
        glDeleteShader(shader);
        return 0;
    }

    GLuint CreateProgram(const std::string& label, const fs::path& vertexPath, const fs::path& fragmentPath) const
    {
        const std::optional<std::string> vertexText = ReadShaderText(vertexPath);
        const std::optional<std::string> fragmentText = ReadShaderText(fragmentPath);
        if (!vertexText || !fragmentText)
        {
            return 0;
        }
        const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, *vertexText, vertexPath);
        if (vertexShader == 0)
        {
            return 0;
        }
        const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, *fragmentText, fragmentPath);
        if (fragmentShader == 0)
        {
            glDeleteShader(vertexShader);
            return 0;
        }

        const GLuint program = glCreateProgram();
        glAttachShader(program, vertexShader);
        glAttachShader(program, fragmentShader);
        glLinkProgram(program);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        GLint succeeded = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &succeeded);
        if (succeeded == GL_TRUE)
        {
            return program;
        }

        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(std::max(logLength, 1));
        glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::cerr << "program 链接失败（" << label << "）：\n" << log.data() << '\n';
        glDeleteProgram(program);
        return 0;
    }

    bool CreatePrograms()
    {
        sceneProgram_ = CreateProgram("scene", "shaders/scene.vert", "shaders/scene.frag");
        shadowProgram_ = CreateProgram("shadow", "shaders/shadow.vert", "shaders/shadow.frag");
        fullscreenProgram_ = CreateProgram("fullscreen", "shaders/fullscreen.vert", "shaders/fullscreen.frag");
        captureProgram_ = CreateProgram("ibl capture", "shaders/ibl_capture.vert", "shaders/environment.frag");
        irradianceProgram_ = CreateProgram("irradiance", "shaders/ibl_capture.vert", "shaders/irradiance.frag");
        prefilterProgram_ = CreateProgram("prefilter", "shaders/ibl_capture.vert", "shaders/prefilter.frag");
        brdfProgram_ = CreateProgram("brdf", "shaders/fullscreen.vert", "shaders/brdf.frag");
        return sceneProgram_ != 0 && shadowProgram_ != 0 && fullscreenProgram_ != 0 && captureProgram_ != 0
            && irradianceProgram_ != 0 && prefilterProgram_ != 0 && brdfProgram_ != 0;
    }

    void BuildStaticInstances()
    {
        instances_.clear();
        instances_.reserve(TotalInstanceCount);
        visibleInstances_[0].reserve(GroundCount);
        visibleInstances_[1].reserve(PillarCount);
        for (int x = -7; x <= 7; ++x)
        {
            for (int z = -7; z <= 7; ++z)
            {
                SceneInstance instance;
                instance.aabbCenter = glm::vec3(static_cast<float>(x), 0.5f, static_cast<float>(z));
                instance.gpu.model = glm::translate(glm::mat4(1.0f), instance.aabbCenter);
                instance.gpu.materialIndex = 0;
                instances_.push_back(instance);
            }
        }

        constexpr std::array<glm::ivec2, 5> pillarPositions{
            glm::ivec2(-7, -7), glm::ivec2(-7, 7), glm::ivec2(7, -7), glm::ivec2(7, 7), glm::ivec2(0, 0)
        };
        for (const glm::ivec2 position : pillarPositions)
        {
            for (int layer = 0; layer < 5; ++layer)
            {
                SceneInstance instance;
                instance.aabbCenter = glm::vec3(static_cast<float>(position.x), 1.5f + static_cast<float>(layer),
                    static_cast<float>(position.y));
                instance.gpu.model = glm::translate(glm::mat4(1.0f), instance.aabbCenter);
                instance.gpu.materialIndex = 1;
                instances_.push_back(instance);
            }
        }

        if (instances_.size() != TotalInstanceCount)
        {
            std::cerr << "内部错误：场景实例数不是 250。\n";
        }
    }

    void ConfigureCubeVao(const GLuint vao, const GLuint instanceBuffer)
    {
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, cubeVertexBuffer_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeIndexBuffer_);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
            reinterpret_cast<const void*>(offsetof(Vertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
            reinterpret_cast<const void*>(offsetof(Vertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
            reinterpret_cast<const void*>(offsetof(Vertex, uv)));

        glBindBuffer(GL_ARRAY_BUFFER, instanceBuffer);
        for (GLuint column = 0; column < 4; ++column)
        {
            glEnableVertexAttribArray(3 + column);
            glVertexAttribPointer(3 + column, 4, GL_FLOAT, GL_FALSE, sizeof(InstanceData),
                reinterpret_cast<const void*>(offsetof(InstanceData, model) + sizeof(glm::vec4) * column));
            glVertexAttribDivisor(3 + column, 1);
        }
        glEnableVertexAttribArray(7);
        glVertexAttribIPointer(7, 1, GL_UNSIGNED_INT, sizeof(InstanceData),
            reinterpret_cast<const void*>(offsetof(InstanceData, materialIndex)));
        glVertexAttribDivisor(7, 1);
        glBindVertexArray(0);
    }

    bool CreateGeometry()
    {
        const std::array<Vertex, 24> vertices{
            Vertex{ {-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f} },
            Vertex{ { 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 0.0f} },
            Vertex{ { 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f} },
            Vertex{ {-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f} },
            Vertex{ { 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f} },
            Vertex{ {-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 0.0f} },
            Vertex{ {-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f} },
            Vertex{ { 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f} },
            Vertex{ {-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            Vertex{ {-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 0.0f} },
            Vertex{ {-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f} },
            Vertex{ {-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f} },
            Vertex{ { 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f} },
            Vertex{ { 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 0.0f} },
            Vertex{ { 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f} },
            Vertex{ { 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f} },
            Vertex{ {-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f} },
            Vertex{ { 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 0.0f} },
            Vertex{ { 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f} },
            Vertex{ {-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 1.0f} },
            Vertex{ {-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f} },
            Vertex{ { 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 0.0f} },
            Vertex{ { 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f} },
            Vertex{ {-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 1.0f} }
        };
        const std::array<std::uint32_t, 36> indices{
            0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4, 8, 9, 10, 10, 11, 8,
            12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20
        };

        std::vector<InstanceData> staticData;
        staticData.reserve(instances_.size());
        for (const SceneInstance& instance : instances_)
        {
            staticData.push_back(instance.gpu);
        }

        glGenBuffers(1, &cubeVertexBuffer_);
        glGenBuffers(1, &cubeIndexBuffer_);
        glGenBuffers(1, &staticInstanceBuffer_);
        glGenBuffers(1, &visibleInstanceBuffer_);
        glBindBuffer(GL_ARRAY_BUFFER, cubeVertexBuffer_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeIndexBuffer_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, staticInstanceBuffer_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(staticData.size() * sizeof(InstanceData)), staticData.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, visibleInstanceBuffer_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(TotalInstanceCount * sizeof(InstanceData)), nullptr, GL_STREAM_DRAW);

        glGenVertexArrays(1, &shadowCubeVao_);
        glGenVertexArrays(1, &visibleCubeVao_);
        ConfigureCubeVao(shadowCubeVao_, staticInstanceBuffer_);
        ConfigureCubeVao(visibleCubeVao_, visibleInstanceBuffer_);
        return true;
    }

    bool CreateFrameUbo()
    {
        glGenBuffers(1, &frameUbo_);
        glBindBuffer(GL_UNIFORM_BUFFER, frameUbo_);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(FrameGpu), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameUbo_);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        return frameUbo_ != 0;
    }

    void GenerateMaterialPixels(const MaterialDefinition& material, std::vector<float>& baseColor,
        std::vector<float>& normal, std::vector<float>& orm, const std::size_t layer) const
    {
        const std::size_t layerOffset = layer * TextureSize * TextureSize * 3;
        for (int y = 0; y < TextureSize; ++y)
        {
            for (int x = 0; x < TextureSize; ++x)
            {
                float patternValue = 0.0f;
                float normalXNoise = 0.0f;
                float normalYNoise = 0.0f;
                if (material.pattern == "brushed_x")
                {
                    const float longStroke = SignedNoise(material.textureSeed, 0, y);
                    const float longitudinalDetail = SignedNoise(material.textureSeed, x / 7, y);
                    patternValue = std::clamp(longStroke * 0.82f + longitudinalDetail * 0.18f, -1.0f, 1.0f);
                    normalXNoise = SignedNoise(material.textureSeed, x / 8, y);
                    normalYNoise = SignedNoise(material.textureSeed, 0, y + 1) * 0.22f;
                }
                else
                {
                    patternValue = SignedNoise(material.textureSeed, x, y);
                    normalXNoise = SignedNoise(material.textureSeed, x + 1, y);
                    normalYNoise = SignedNoise(material.textureSeed, x, y + 1);
                }

                const float normalX = std::clamp(normalXNoise * material.normalStrength,
                    -material.normalStrength, material.normalStrength);
                const float normalY = std::clamp(normalYNoise * material.normalStrength,
                    -material.normalStrength, material.normalStrength);
                const float normalZ = std::sqrt(std::max(0.0f, 1.0f - normalX * normalX - normalY * normalY));
                const float roughness = std::clamp(material.roughness + patternValue * material.roughnessVariation, 0.0f, 1.0f);
                const std::size_t texelOffset = layerOffset + static_cast<std::size_t>(y * TextureSize + x) * 3;

                for (int component = 0; component < 3; ++component)
                {
                    baseColor[texelOffset + component] = std::clamp(material.baseColorSrgb[component]
                        + patternValue * material.baseColorVariation, 0.0f, 1.0f);
                }
                normal[texelOffset] = normalX * 0.5f + 0.5f;
                normal[texelOffset + 1] = normalY * 0.5f + 0.5f;
                normal[texelOffset + 2] = normalZ * 0.5f + 0.5f;
                orm[texelOffset] = material.ambientOcclusion;
                orm[texelOffset + 1] = roughness;
                orm[texelOffset + 2] = material.metallic;
            }
        }
    }

    bool CreateMaterialTextures()
    {
        const std::size_t texelCount = static_cast<std::size_t>(MaterialCount) * TextureSize * TextureSize * 3;
        std::vector<float> baseColor(texelCount);
        std::vector<float> normal(texelCount);
        std::vector<float> orm(texelCount);
        for (std::size_t layer = 0; layer < MaterialCount; ++layer)
        {
            GenerateMaterialPixels(materials_[layer], baseColor, normal, orm, layer);
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glGenTextures(1, &baseColorArray_);
        glBindTexture(GL_TEXTURE_2D_ARRAY, baseColorArray_);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8, TextureSize, TextureSize, MaterialCount, 0, GL_RGB, GL_FLOAT, baseColor.data());
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

        glGenTextures(1, &normalArray_);
        glBindTexture(GL_TEXTURE_2D_ARRAY, normalArray_);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB16F, TextureSize, TextureSize, MaterialCount, 0, GL_RGB, GL_FLOAT, normal.data());
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);

        glGenTextures(1, &ormArray_);
        glBindTexture(GL_TEXTURE_2D_ARRAY, ormArray_);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB16F, TextureSize, TextureSize, MaterialCount, 0, GL_RGB, GL_FLOAT, orm.data());
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        return baseColorArray_ != 0 && normalArray_ != 0 && ormArray_ != 0;
    }

    bool CheckFramebuffer(const std::string& label) const
    {
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status == GL_FRAMEBUFFER_COMPLETE)
        {
            return true;
        }
        std::cerr << "framebuffer 不完整（" << label << "），状态：0x" << std::hex << status << std::dec << '\n';
        return false;
    }

    bool CreateSceneFramebuffer()
    {
        glGenFramebuffers(1, &sceneFramebuffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);

        glGenTextures(1, &sceneColorTexture_);
        glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, SceneWidth, SceneHeight, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColorTexture_, 0);

        glGenTextures(1, &sceneDepthTexture_);
        glBindTexture(GL_TEXTURE_2D, sceneDepthTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, SceneWidth, SceneHeight, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sceneDepthTexture_, 0);
        const GLenum drawBuffers[] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, drawBuffers);
        const bool complete = CheckFramebuffer("640x360 场景目标");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return complete;
    }

    bool CreateShadowFramebuffer()
    {
        glGenFramebuffers(1, &shadowFramebuffer_);
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
        glGenTextures(1, &shadowDepthTexture_);
        glBindTexture(GL_TEXTURE_2D, shadowDepthTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, ShadowMapSize, ShadowMapSize, 0,
            GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const std::array<float, 4> borderColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor.data());
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowDepthTexture_, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        const bool complete = CheckFramebuffer("2048x2048 directional shadow map");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return complete;
    }

    bool CreateFullscreenVao()
    {
        glGenVertexArrays(1, &fullscreenVao_);
        return fullscreenVao_ != 0;
    }

    void ConfigureCubemap(const GLuint texture, const GLint minFilter, const GLint magFilter, const GLint maxLevel = 0) const
    {
        glBindTexture(GL_TEXTURE_CUBE_MAP, texture);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, magFilter);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, maxLevel);
    }

    bool CreateIblTextures()
    {
        glGenFramebuffers(1, &captureFramebuffer_);
        glGenRenderbuffers(1, &captureDepthRenderbuffer_);

        glGenTextures(1, &environmentCubemap_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, environmentCubemap_);
        for (int face = 0; face < 6; ++face)
        {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB16F, EnvironmentSize, EnvironmentSize,
                0, GL_RGB, GL_FLOAT, nullptr);
        }
        ConfigureCubemap(environmentCubemap_, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, 8);

        glGenTextures(1, &irradianceCubemap_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, irradianceCubemap_);
        for (int face = 0; face < 6; ++face)
        {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGB16F, IrradianceSize, IrradianceSize,
                0, GL_RGB, GL_FLOAT, nullptr);
        }
        ConfigureCubemap(irradianceCubemap_, GL_LINEAR, GL_LINEAR);

        glGenTextures(1, &prefilterCubemap_);
        glBindTexture(GL_TEXTURE_CUBE_MAP, prefilterCubemap_);
        glTexStorage2D(GL_TEXTURE_CUBE_MAP, PrefilterMipCount, GL_RGB16F, PrefilterSize, PrefilterSize);
        ConfigureCubemap(prefilterCubemap_, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, PrefilterMipCount - 1);

        glGenTextures(1, &brdfLutTexture_);
        glBindTexture(GL_TEXTURE_2D, brdfLutTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, BrdfLutSize, BrdfLutSize, 0, GL_RG, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        return environmentCubemap_ != 0 && irradianceCubemap_ != 0 && prefilterCubemap_ != 0 && brdfLutTexture_ != 0;
    }

    void SetMatrixUniform(const GLuint program, const char* name, const glm::mat4& matrix) const
    {
        const GLint location = glGetUniformLocation(program, name);
        if (location >= 0)
        {
            glUniformMatrix4fv(location, 1, GL_FALSE, glm::value_ptr(matrix));
        }
    }

    void SetFloatUniform(const GLuint program, const char* name, const float value) const
    {
        const GLint location = glGetUniformLocation(program, name);
        if (location >= 0)
        {
            glUniform1f(location, value);
        }
    }

    void SetIntUniform(const GLuint program, const char* name, const int value) const
    {
        const GLint location = glGetUniformLocation(program, name);
        if (location >= 0)
        {
            glUniform1i(location, value);
        }
    }

    bool GenerateImageBasedLighting()
    {
        const glm::mat4 captureProjection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
        const std::array<glm::mat4, 6> captureViews{
            glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f))
        };

        glBindFramebuffer(GL_FRAMEBUFFER, captureFramebuffer_);
        glBindRenderbuffer(GL_RENDERBUFFER, captureDepthRenderbuffer_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, EnvironmentSize, EnvironmentSize);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, captureDepthRenderbuffer_);
        glDisable(GL_CULL_FACE);
        glViewport(0, 0, EnvironmentSize, EnvironmentSize);
        glUseProgram(captureProgram_);
        SetMatrixUniform(captureProgram_, "uProjection", captureProjection);
        glBindVertexArray(shadowCubeVao_);
        for (int face = 0; face < 6; ++face)
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                environmentCubemap_, 0);
            if (!CheckFramebuffer("procedural environment cubemap"))
            {
                return false;
            }
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            SetMatrixUniform(captureProgram_, "uView", captureViews[face]);
            glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr);
        }
        glBindTexture(GL_TEXTURE_CUBE_MAP, environmentCubemap_);
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);

        glBindRenderbuffer(GL_RENDERBUFFER, captureDepthRenderbuffer_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, IrradianceSize, IrradianceSize);
        glViewport(0, 0, IrradianceSize, IrradianceSize);
        glUseProgram(irradianceProgram_);
        SetMatrixUniform(irradianceProgram_, "uProjection", captureProjection);
        SetIntUniform(irradianceProgram_, "uEnvironment", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, environmentCubemap_);
        for (int face = 0; face < 6; ++face)
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                irradianceCubemap_, 0);
            if (!CheckFramebuffer("irradiance cubemap"))
            {
                return false;
            }
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            SetMatrixUniform(irradianceProgram_, "uView", captureViews[face]);
            glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr);
        }

        glUseProgram(prefilterProgram_);
        SetMatrixUniform(prefilterProgram_, "uProjection", captureProjection);
        SetIntUniform(prefilterProgram_, "uEnvironment", 0);
        SetFloatUniform(prefilterProgram_, "uEnvironmentResolution", static_cast<float>(EnvironmentSize));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, environmentCubemap_);
        for (int mip = 0; mip < PrefilterMipCount; ++mip)
        {
            const int mipSize = std::max(1, PrefilterSize >> mip);
            glBindRenderbuffer(GL_RENDERBUFFER, captureDepthRenderbuffer_);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, mipSize, mipSize);
            glViewport(0, 0, mipSize, mipSize);
            SetFloatUniform(prefilterProgram_, "uRoughness", static_cast<float>(mip) / static_cast<float>(PrefilterMipCount - 1));
            for (int face = 0; face < 6; ++face)
            {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                    prefilterCubemap_, mip);
                if (!CheckFramebuffer("GGX prefilter cubemap"))
                {
                    return false;
                }
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                SetMatrixUniform(prefilterProgram_, "uView", captureViews[face]);
                glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr);
            }
        }

        glBindRenderbuffer(GL_RENDERBUFFER, captureDepthRenderbuffer_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, BrdfLutSize, BrdfLutSize);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, brdfLutTexture_, 0);
        if (!CheckFramebuffer("BRDF integration LUT"))
        {
            return false;
        }
        glViewport(0, 0, BrdfLutSize, BrdfLutSize);
        glUseProgram(brdfProgram_);
        glBindVertexArray(fullscreenVao_);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_CULL_FACE);

        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDepthFunc(GL_LESS);
        return true;
    }

    void ConfigureSamplerBindings() const
    {
        glUseProgram(sceneProgram_);
        SetIntUniform(sceneProgram_, "uBaseColor", 0);
        SetIntUniform(sceneProgram_, "uNormalMap", 1);
        SetIntUniform(sceneProgram_, "uOrm", 2);
        SetIntUniform(sceneProgram_, "uShadowMap", 3);
        SetIntUniform(sceneProgram_, "uIrradiance", 4);
        SetIntUniform(sceneProgram_, "uPrefilter", 5);
        SetIntUniform(sceneProgram_, "uBrdfLut", 6);
        glUseProgram(fullscreenProgram_);
        SetIntUniform(fullscreenProgram_, "uSceneColor", 0);
        glUseProgram(0);
    }

    std::array<Plane, 6> ExtractFrustumPlanes(const glm::mat4& clip) const
    {
        const glm::vec4 row0(clip[0][0], clip[1][0], clip[2][0], clip[3][0]);
        const glm::vec4 row1(clip[0][1], clip[1][1], clip[2][1], clip[3][1]);
        const glm::vec4 row2(clip[0][2], clip[1][2], clip[2][2], clip[3][2]);
        const glm::vec4 row3(clip[0][3], clip[1][3], clip[2][3], clip[3][3]);
        const std::array<glm::vec4, 6> rawPlanes{
            row3 + row0, row3 - row0, row3 + row1, row3 - row1, row3 + row2, row3 - row2
        };

        std::array<Plane, 6> planes{};
        for (std::size_t index = 0; index < rawPlanes.size(); ++index)
        {
            const float length = glm::length(glm::vec3(rawPlanes[index]));
            planes[index].normal = glm::vec3(rawPlanes[index]) / length;
            planes[index].distance = rawPlanes[index].w / length;
        }
        return planes;
    }

    bool IsAabbVisible(const SceneInstance& instance, const std::array<Plane, 6>& planes) const
    {
        constexpr glm::vec3 halfExtent(0.5f);
        for (const Plane& plane : planes)
        {
            const float projectedRadius = glm::dot(glm::abs(plane.normal), halfExtent);
            if (glm::dot(plane.normal, instance.aabbCenter) + plane.distance + projectedRadius < 0.0f)
            {
                return false;
            }
        }
        return true;
    }

    void BuildVisibleInstances(const glm::mat4& projectionView)
    {
        for (std::vector<InstanceData>& visible : visibleInstances_)
        {
            visible.clear();
        }
        const std::array<Plane, 6> planes = ExtractFrustumPlanes(projectionView);
        for (const SceneInstance& instance : instances_)
        {
            if (!cullingEnabled_ || IsAabbVisible(instance, planes))
            {
                visibleInstances_[instance.gpu.materialIndex].push_back(instance.gpu);
            }
        }
        visibleSubmissionCount_ = static_cast<int>(visibleInstances_[0].size() + visibleInstances_[1].size());
    }

    void UploadVisibleInstances(const std::vector<InstanceData>& visible) const
    {
        if (visible.empty())
        {
            return;
        }
        glBindBuffer(GL_ARRAY_BUFFER, visibleInstanceBuffer_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(TotalInstanceCount * sizeof(InstanceData)), nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(visible.size() * sizeof(InstanceData)), visible.data());
    }

    void UpdateFrameData(const glm::mat4& view, const glm::mat4& projection)
    {
        FrameGpu data;
        data.view = view;
        data.projection = projection;
        const glm::vec3 sunRayDirection = glm::normalize(glm::vec3(-0.45f, -1.0f, -0.35f));
        const glm::vec3 lightTarget(0.0f, 2.5f, 0.0f);
        const glm::vec3 lightPosition = lightTarget - sunRayDirection * 24.0f;
        const glm::mat4 lightView = glm::lookAt(lightPosition, lightTarget, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 lightProjection = glm::ortho(-13.0f, 13.0f, -13.0f, 13.0f, 0.1f, 50.0f);
        data.lightSpace = lightProjection * lightView;
        data.cameraPosition = glm::vec4(camera_.position, 1.0f);
        data.sunDirection = glm::vec4(sunRayDirection, 0.0f);
        data.sunRadiance = glm::vec4(4.5f, 4.2f, 3.8f, 0.0f);
        data.pointPositions = {
            glm::vec4(-5.0f, 6.5f, -5.0f, 0.0f), glm::vec4(-5.0f, 6.5f, 5.0f, 0.0f),
            glm::vec4(5.0f, 6.5f, -5.0f, 0.0f), glm::vec4(5.0f, 6.5f, 5.0f, 0.0f)
        };
        data.pointRadiances = {
            glm::vec4(35.0f, 19.25f, 9.8f, 0.0f), glm::vec4(9.8f, 19.25f, 35.0f, 0.0f),
            glm::vec4(35.0f, 19.25f, 9.8f, 0.0f), glm::vec4(9.8f, 19.25f, 35.0f, 0.0f)
        };
        data.settings = glm::ivec4(debugMode_, 0, 0, 0);
        lightSpaceMatrix_ = data.lightSpace;

        glBindBuffer(GL_UNIFORM_BUFFER, frameUbo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(FrameGpu), &data);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    void RenderShadowPass() const
    {
        glViewport(0, 0, ShadowMapSize, ShadowMapSize);
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFramebuffer_);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glClear(GL_DEPTH_BUFFER_BIT);
        glUseProgram(shadowProgram_);
        SetMatrixUniform(shadowProgram_, "uLightSpace", lightSpaceMatrix_);
        glBindVertexArray(shadowCubeVao_);
        glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr, TotalInstanceCount);
    }

    void BindSceneTextures() const
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, baseColorArray_);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, normalArray_);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, ormArray_);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, shadowDepthTexture_);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_CUBE_MAP, irradianceCubemap_);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_CUBE_MAP, prefilterCubemap_);
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, brdfLutTexture_);
    }

    void RenderScenePass()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, sceneFramebuffer_);
        glViewport(0, 0, SceneWidth, SceneHeight);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glClearColor(0.012f, 0.016f, 0.026f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(sceneProgram_);
        BindSceneTextures();
        glBindVertexArray(visibleCubeVao_);

        mainCubeDrawCalls_ = 0;
        for (const std::vector<InstanceData>& visible : visibleInstances_)
        {
            if (visible.empty())
            {
                continue;
            }
            UploadVisibleInstances(visible);
            glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(visible.size()));
            ++mainCubeDrawCalls_;
        }
    }

    void RenderUpscalePass() const
    {
        if (framebufferWidth_ <= 0 || framebufferHeight_ <= 0)
        {
            return;
        }
        const float targetAspect = static_cast<float>(SceneWidth) / static_cast<float>(SceneHeight);
        int viewportWidth = framebufferWidth_;
        int viewportHeight = static_cast<int>(std::round(static_cast<float>(viewportWidth) / targetAspect));
        if (viewportHeight > framebufferHeight_)
        {
            viewportHeight = framebufferHeight_;
            viewportWidth = static_cast<int>(std::round(static_cast<float>(viewportHeight) * targetAspect));
        }
        const int viewportX = (framebufferWidth_ - viewportWidth) / 2;
        const int viewportY = (framebufferHeight_ - viewportHeight) / 2;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, framebufferWidth_, framebufferHeight_);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
        glUseProgram(fullscreenProgram_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneColorTexture_);
        glBindVertexArray(fullscreenVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    void RenderFrame()
    {
        const glm::mat4 view = camera_.ViewMatrix();
        const glm::mat4 projection = glm::perspective(glm::radians(60.0f),
            static_cast<float>(SceneWidth) / static_cast<float>(SceneHeight), 0.1f, 100.0f);
        BuildVisibleInstances(projection * view);
        UpdateFrameData(view, projection);
        RenderShadowPass();
        RenderScenePass();
        RenderUpscalePass();
    }

    void ProcessMovement(const float deltaTime)
    {
        if (!mouseCaptured_)
        {
            return;
        }
        const float speed = 7.0f * deltaTime;
        float forward = 0.0f;
        float right = 0.0f;
        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS)
        {
            forward += speed;
        }
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS)
        {
            forward -= speed;
        }
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS)
        {
            right += speed;
        }
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS)
        {
            right -= speed;
        }
        camera_.Move(forward, right);
    }

    void SetMouseCapture(const bool capture)
    {
        mouseCaptured_ = capture;
        firstMouseSample_ = true;
        glfwSetInputMode(window_, GLFW_CURSOR, capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }

    void UpdateWindowTitle(const double fps)
    {
        static constexpr std::array<const char*, 5> debugNames{
            "PBR", "Albedo", "World Normal", "Roughness/Metallic", "Shadow Factor"
        };
        std::ostringstream title;
        title << std::fixed << std::setprecision(1);
        title << "OpenGL VoxelPBRFrustumCulling Test | FPS: " << fps
            << " | Visible: " << visibleSubmissionCount_ << '/' << TotalInstanceCount
            << " | Culled: " << (TotalInstanceCount - visibleSubmissionCount_)
            << " | Cube draws: " << mainCubeDrawCalls_
            << " | Culling: " << (cullingEnabled_ ? "On" : "Off")
            << " | Debug: " << debugNames[static_cast<std::size_t>(debugMode_ - 1)];
        glfwSetWindowTitle(window_, title.str().c_str());
    }

    void HandleKey(const int key, const int action)
    {
        if (action != GLFW_PRESS)
        {
            return;
        }
        if (key == GLFW_KEY_ESCAPE)
        {
            if (mouseCaptured_)
            {
                SetMouseCapture(false);
            }
            else
            {
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            }
            return;
        }
        if (key == GLFW_KEY_C)
        {
            cullingEnabled_ = !cullingEnabled_;
            return;
        }
        if (key >= GLFW_KEY_1 && key <= GLFW_KEY_5)
        {
            debugMode_ = key - GLFW_KEY_1 + 1;
        }
    }

    static void FramebufferSizeCallback(GLFWwindow* window, const int width, const int height)
    {
        auto* application = static_cast<Application*>(glfwGetWindowUserPointer(window));
        if (application != nullptr)
        {
            application->framebufferWidth_ = width;
            application->framebufferHeight_ = height;
        }
    }

    static void CursorPositionCallback(GLFWwindow* window, const double xPosition, const double yPosition)
    {
        auto* application = static_cast<Application*>(glfwGetWindowUserPointer(window));
        if (application == nullptr || !application->mouseCaptured_)
        {
            return;
        }
        if (application->firstMouseSample_)
        {
            application->lastMouseX_ = xPosition;
            application->lastMouseY_ = yPosition;
            application->firstMouseSample_ = false;
            return;
        }
        const float xOffset = static_cast<float>(xPosition - application->lastMouseX_) * 0.12f;
        const float yOffset = static_cast<float>(application->lastMouseY_ - yPosition) * 0.12f;
        application->lastMouseX_ = xPosition;
        application->lastMouseY_ = yPosition;
        application->camera_.Rotate(xOffset, yOffset);
    }

    static void MouseButtonCallback(GLFWwindow* window, const int button, const int action, const int)
    {
        auto* application = static_cast<Application*>(glfwGetWindowUserPointer(window));
        if (application != nullptr && button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS
            && !application->mouseCaptured_)
        {
            application->SetMouseCapture(true);
        }
    }

    static void KeyCallback(GLFWwindow* window, const int key, const int, const int action, const int)
    {
        auto* application = static_cast<Application*>(glfwGetWindowUserPointer(window));
        if (application != nullptr)
        {
            application->HandleKey(key, action);
        }
    }

    void Shutdown()
    {
        if (glReady_)
        {
            const std::array<GLuint, 7> programs{
                sceneProgram_, shadowProgram_, fullscreenProgram_, captureProgram_, irradianceProgram_, prefilterProgram_, brdfProgram_
            };
            for (const GLuint program : programs)
            {
                if (program != 0)
                {
                    glDeleteProgram(program);
                }
            }
            const std::array<GLuint, 9> textures{
                baseColorArray_, normalArray_, ormArray_, sceneColorTexture_, sceneDepthTexture_, shadowDepthTexture_,
                environmentCubemap_, irradianceCubemap_, prefilterCubemap_
            };
            for (const GLuint texture : textures)
            {
                if (texture != 0)
                {
                    glDeleteTextures(1, &texture);
                }
            }
            if (brdfLutTexture_ != 0)
            {
                glDeleteTextures(1, &brdfLutTexture_);
            }
            const std::array<GLuint, 5> buffers{
                cubeVertexBuffer_, cubeIndexBuffer_, staticInstanceBuffer_, visibleInstanceBuffer_, frameUbo_
            };
            for (const GLuint buffer : buffers)
            {
                if (buffer != 0)
                {
                    glDeleteBuffers(1, &buffer);
                }
            }
            const std::array<GLuint, 3> vaos{ shadowCubeVao_, visibleCubeVao_, fullscreenVao_ };
            for (const GLuint vao : vaos)
            {
                if (vao != 0)
                {
                    glDeleteVertexArrays(1, &vao);
                }
            }
            const std::array<GLuint, 3> framebuffers{ sceneFramebuffer_, shadowFramebuffer_, captureFramebuffer_ };
            for (const GLuint framebuffer : framebuffers)
            {
                if (framebuffer != 0)
                {
                    glDeleteFramebuffers(1, &framebuffer);
                }
            }
            if (captureDepthRenderbuffer_ != 0)
            {
                glDeleteRenderbuffers(1, &captureDepthRenderbuffer_);
            }
        }
        if (window_ != nullptr)
        {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
        if (glfwInitialized_)
        {
            glfwTerminate();
            glfwInitialized_ = false;
        }
    }

    GLFWwindow* window_ = nullptr;
    bool glfwInitialized_ = false;
    bool glReady_ = false;
    fs::path projectDirectory_;
    fs::path materialsPath_;
    std::array<MaterialDefinition, MaterialCount> materials_{};
    std::vector<SceneInstance> instances_;
    std::array<std::vector<InstanceData>, MaterialCount> visibleInstances_;
    Camera camera_;

    GLuint sceneProgram_ = 0;
    GLuint shadowProgram_ = 0;
    GLuint fullscreenProgram_ = 0;
    GLuint captureProgram_ = 0;
    GLuint irradianceProgram_ = 0;
    GLuint prefilterProgram_ = 0;
    GLuint brdfProgram_ = 0;
    GLuint cubeVertexBuffer_ = 0;
    GLuint cubeIndexBuffer_ = 0;
    GLuint staticInstanceBuffer_ = 0;
    GLuint visibleInstanceBuffer_ = 0;
    GLuint shadowCubeVao_ = 0;
    GLuint visibleCubeVao_ = 0;
    GLuint fullscreenVao_ = 0;
    GLuint frameUbo_ = 0;
    GLuint baseColorArray_ = 0;
    GLuint normalArray_ = 0;
    GLuint ormArray_ = 0;
    GLuint sceneFramebuffer_ = 0;
    GLuint sceneColorTexture_ = 0;
    GLuint sceneDepthTexture_ = 0;
    GLuint shadowFramebuffer_ = 0;
    GLuint shadowDepthTexture_ = 0;
    GLuint captureFramebuffer_ = 0;
    GLuint captureDepthRenderbuffer_ = 0;
    GLuint environmentCubemap_ = 0;
    GLuint irradianceCubemap_ = 0;
    GLuint prefilterCubemap_ = 0;
    GLuint brdfLutTexture_ = 0;

    glm::mat4 lightSpaceMatrix_{ 1.0f };
    int framebufferWidth_ = WindowWidth;
    int framebufferHeight_ = WindowHeight;
    bool mouseCaptured_ = false;
    bool firstMouseSample_ = true;
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;
    bool cullingEnabled_ = true;
    int debugMode_ = 1;
    int visibleSubmissionCount_ = TotalInstanceCount;
    int mainCubeDrawCalls_ = 0;
};
}

int main()
{
    Application application;
    return application.Run() ? 0 : 1;
}
