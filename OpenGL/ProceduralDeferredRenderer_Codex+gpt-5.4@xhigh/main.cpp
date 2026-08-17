#define GLFW_INCLUDE_NONE

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr int kInitialWindowWidth = 1600;
constexpr int kInitialWindowHeight = 900;
constexpr int kShadowMapSize = 2048;
constexpr int kPointLightCount = 16;
constexpr int kBloomBlurPassCount = 8;
constexpr char kWindowTitleBase[] = "OpenGL ProceduralDeferredRenderer Test";

enum class DebugView : int
{
    Final = 0,
    Normals = 1,
    ShadowMap = 2,
};

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct Mesh
{
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
};

struct InstanceData
{
    glm::mat4 model{1.0f};
    glm::vec4 baseColor{1.0f};
    glm::vec4 material{0.0f};
};

struct Batch
{
    std::string label;
    Mesh mesh;
    GLuint instanceBuffer = 0;
    std::vector<InstanceData> instances;
};

struct PointLightGpu
{
    glm::vec4 positionRadius{0.0f};
    glm::vec4 colorIntensity{0.0f};
};

struct alignas(16) FrameDataGpu
{
    glm::mat4 view{1.0f};
    glm::mat4 proj{1.0f};
    glm::mat4 lightViewProj{1.0f};
    glm::vec4 cameraPos{0.0f};
    glm::vec4 dirLightDirection{0.0f};
    glm::vec4 dirLightColorIntensity{0.0f};
    glm::vec4 screenData{0.0f};
    glm::vec4 featureFlags{0.0f};
};

struct CachedPass
{
    std::string name;
    std::string target;
    std::function<void()> execute;
};

struct Program
{
    GLuint id = 0;

    void use() const
    {
        glUseProgram(id);
    }

    void setInt(const char* name, int value) const
    {
        glUniform1i(glGetUniformLocation(id, name), value);
    }

    void setFloat(const char* name, float value) const
    {
        glUniform1f(glGetUniformLocation(id, name), value);
    }

    void setBool(const char* name, bool value) const
    {
        setInt(name, value ? 1 : 0);
    }
};

struct Camera
{
    glm::vec3 position{0.0f, 6.5f, 24.0f};
    float yaw = -90.0f;
    float pitch = -15.0f;
    float verticalFovDegrees = 60.0f;

    [[nodiscard]] glm::vec3 front() const
    {
        const float yawRadians = glm::radians(yaw);
        const float pitchRadians = glm::radians(pitch);
        return glm::normalize(glm::vec3(
            std::cos(yawRadians) * std::cos(pitchRadians),
            std::sin(pitchRadians),
            std::sin(yawRadians) * std::cos(pitchRadians)));
    }

    [[nodiscard]] glm::mat4 viewMatrix() const
    {
        return glm::lookAt(position, position + front(), glm::vec3(0.0f, 1.0f, 0.0f));
    }
};

[[nodiscard]] std::string glErrorString(GLenum status)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << status;
    return stream.str();
}

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

void deleteBuffer(GLuint& id)
{
    if (id != 0)
    {
        glDeleteBuffers(1, &id);
        id = 0;
    }
}

void deleteVertexArray(GLuint& id)
{
    if (id != 0)
    {
        glDeleteVertexArrays(1, &id);
        id = 0;
    }
}

void deleteTexture(GLuint& id)
{
    if (id != 0)
    {
        glDeleteTextures(1, &id);
        id = 0;
    }
}

void deleteFramebuffer(GLuint& id)
{
    if (id != 0)
    {
        glDeleteFramebuffers(1, &id);
        id = 0;
    }
}

void deleteRenderbuffer(GLuint& id)
{
    if (id != 0)
    {
        glDeleteRenderbuffers(1, &id);
        id = 0;
    }
}

void deleteProgram(GLuint& id)
{
    if (id != 0)
    {
        glDeleteProgram(id);
        id = 0;
    }
}

GLuint compileShader(GLenum shaderType, const char* source, std::string_view label)
{
    const GLuint shader = glCreateShader(shaderType);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
    {
        return shader;
    }

    GLint logLength = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
    glGetShaderInfoLog(shader, logLength, nullptr, log.data());

    std::ostringstream stream;
    stream << "Failed to compile " << label << " shader.\n" << log;
    glDeleteShader(shader);
    fail(stream.str());
}

Program createProgram(const char* vertexSource, const char* fragmentSource, std::string_view label)
{
    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource, std::string(label) + " vertex");
    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource, std::string(label) + " fragment");

    Program program{};
    program.id = glCreateProgram();
    glAttachShader(program.id, vertexShader);
    glAttachShader(program.id, fragmentShader);
    glLinkProgram(program.id);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program.id, GL_LINK_STATUS, &linked);
    if (linked == GL_TRUE)
    {
        return program;
    }

    GLint logLength = 0;
    glGetProgramiv(program.id, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
    glGetProgramInfoLog(program.id, logLength, nullptr, log.data());

    std::ostringstream stream;
    stream << "Failed to link " << label << " program.\n" << log;
    deleteProgram(program.id);
    fail(stream.str());
}

void checkFramebufferComplete(std::string_view label)
{
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        std::ostringstream stream;
        stream << "Framebuffer '" << label << "' is incomplete. Status: " << glErrorString(status);
        fail(stream.str());
    }
}

glm::vec3 hsvToRgb(float h, float s, float v)
{
    h = h - std::floor(h);
    const float chroma = v * s;
    const float scaled = h * 6.0f;
    const float x = chroma * (1.0f - std::fabs(std::fmod(scaled, 2.0f) - 1.0f));

    glm::vec3 rgb(0.0f);
    if (scaled < 1.0f)
    {
        rgb = glm::vec3(chroma, x, 0.0f);
    }
    else if (scaled < 2.0f)
    {
        rgb = glm::vec3(x, chroma, 0.0f);
    }
    else if (scaled < 3.0f)
    {
        rgb = glm::vec3(0.0f, chroma, x);
    }
    else if (scaled < 4.0f)
    {
        rgb = glm::vec3(0.0f, x, chroma);
    }
    else if (scaled < 5.0f)
    {
        rgb = glm::vec3(x, 0.0f, chroma);
    }
    else
    {
        rgb = glm::vec3(chroma, 0.0f, x);
    }

    const float match = v - chroma;
    return rgb + glm::vec3(match);
}

constexpr const char* kGeometryVertexShader = R"(#version 460 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
layout (location = 3) in vec4 aModel0;
layout (location = 4) in vec4 aModel1;
layout (location = 5) in vec4 aModel2;
layout (location = 6) in vec4 aModel3;
layout (location = 7) in vec4 aBaseColor;
layout (location = 8) in vec4 aMaterial;

layout (std140, binding = 0) uniform FrameData
{
    mat4 uView;
    mat4 uProj;
    mat4 uLightViewProj;
    vec4 uCameraPos;
    vec4 uDirLightDirection;
    vec4 uDirLightColorIntensity;
    vec4 uScreenData;
    vec4 uFeatureFlags;
};

out VS_OUT
{
    vec3 worldPos;
    vec3 normal;
    vec2 uv;
    vec4 baseColor;
    vec4 material;
} vsOut;

void main()
{
    mat4 model = mat4(aModel0, aModel1, aModel2, aModel3);
    vec4 world = model * vec4(aPosition, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(model)));

    vsOut.worldPos = world.xyz;
    vsOut.normal = normalize(normalMatrix * aNormal);
    vsOut.uv = aUv;
    vsOut.baseColor = aBaseColor;
    vsOut.material = aMaterial;

    gl_Position = uProj * uView * world;
}
)";

constexpr const char* kGeometryFragmentShader = R"(#version 460 core
layout (location = 0) out vec4 gPositionMetallic;
layout (location = 1) out vec4 gNormalRoughness;
layout (location = 2) out vec4 gAlbedoAo;

in VS_OUT
{
    vec3 worldPos;
    vec3 normal;
    vec2 uv;
    vec4 baseColor;
    vec4 material;
} fsIn;

uniform sampler2D uCheckerTex;
uniform sampler2D uStripeTex;
uniform sampler2D uNoiseTex;

vec3 sampleMaterialTexture(int materialId, vec2 uv, vec3 worldPos)
{
    if (materialId == 0)
    {
        return texture(uCheckerTex, uv).rgb;
    }

    if (materialId == 1)
    {
        vec2 warpedUv = uv * vec2(0.9, 1.5) + worldPos.xz * 0.03;
        vec3 stripes = texture(uStripeTex, warpedUv).rgb;
        vec3 checker = texture(uCheckerTex, uv * 0.35).rgb;
        return mix(stripes, checker, 0.3);
    }

    vec2 noiseUv = worldPos.xz * 0.11 + uv * 0.55;
    vec3 noiseColor = texture(uNoiseTex, noiseUv).rgb;
    return mix(vec3(0.76, 0.80, 0.84), noiseColor, 0.7);
}

void main()
{
    float uvScale = max(fsIn.material.w, 0.001);
    vec2 tiledUv = fsIn.uv * uvScale;
    int materialId = int(round(fsIn.material.x));

    vec3 texColor = sampleMaterialTexture(materialId, tiledUv, fsIn.worldPos);
    vec3 albedo = clamp(texColor * fsIn.baseColor.rgb, vec3(0.03), vec3(1.0));
    float roughness = clamp(fsIn.material.y, 0.08, 1.0);
    float metallic = clamp(fsIn.material.z, 0.0, 1.0);

    gPositionMetallic = vec4(fsIn.worldPos, metallic);
    gNormalRoughness = vec4(normalize(fsIn.normal), roughness);
    gAlbedoAo = vec4(albedo, 1.0);
}
)";

constexpr const char* kShadowVertexShader = R"(#version 460 core
layout (location = 0) in vec3 aPosition;
layout (location = 3) in vec4 aModel0;
layout (location = 4) in vec4 aModel1;
layout (location = 5) in vec4 aModel2;
layout (location = 6) in vec4 aModel3;

layout (std140, binding = 0) uniform FrameData
{
    mat4 uView;
    mat4 uProj;
    mat4 uLightViewProj;
    vec4 uCameraPos;
    vec4 uDirLightDirection;
    vec4 uDirLightColorIntensity;
    vec4 uScreenData;
    vec4 uFeatureFlags;
};

void main()
{
    mat4 model = mat4(aModel0, aModel1, aModel2, aModel3);
    gl_Position = uLightViewProj * model * vec4(aPosition, 1.0);
}
)";

constexpr const char* kShadowFragmentShader = R"(#version 460 core
void main()
{
}
)";

constexpr const char* kFullscreenTriangleVertexShader = R"(#version 460 core
out vec2 vUv;

void main()
{
    const vec2 positions[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0));

    vec2 position = positions[gl_VertexID];
    vUv = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

constexpr const char* kLightingFragmentShader = R"(#version 460 core
layout (location = 0) out vec4 oSceneColor;
layout (location = 1) out vec4 oBrightColor;

in vec2 vUv;

layout (std140, binding = 0) uniform FrameData
{
    mat4 uView;
    mat4 uProj;
    mat4 uLightViewProj;
    vec4 uCameraPos;
    vec4 uDirLightDirection;
    vec4 uDirLightColorIntensity;
    vec4 uScreenData;
    vec4 uFeatureFlags;
};

struct PointLight
{
    vec4 positionRadius;
    vec4 colorIntensity;
};

layout (std430, binding = 1) readonly buffer PointLightBuffer
{
    PointLight uPointLights[];
};

uniform sampler2D uGPositionMetallic;
uniform sampler2D uGNormalRoughness;
uniform sampler2D uGAlbedoAo;
uniform sampler2D uShadowMap;
uniform int uPointLightCount;

float sampleShadow(vec3 worldPos, vec3 normal)
{
    vec4 lightClip = uLightViewProj * vec4(worldPos, 1.0);
    vec3 projected = lightClip.xyz / lightClip.w;
    projected = projected * 0.5 + 0.5;

    if (projected.z > 1.0 || projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0)
    {
        return 0.0;
    }

    vec3 lightVector = normalize(-uDirLightDirection.xyz);
    float bias = max(0.0025 * (1.0 - dot(normal, lightVector)), 0.0007);
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));

    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float sampleDepth = texture(uShadowMap, projected.xy + vec2(x, y) * texelSize).r;
            shadow += projected.z - bias > sampleDepth ? 1.0 : 0.0;
        }
    }

    return shadow / 9.0;
}

vec3 shadePointLight(PointLight light, vec3 worldPos, vec3 normal, vec3 viewDir, vec3 albedo, float roughness, float metallic)
{
    vec3 toLight = light.positionRadius.xyz - worldPos;
    float distanceToLight = length(toLight);
    float radius = light.positionRadius.w;
    if (distanceToLight <= 0.0001 || distanceToLight > radius)
    {
        return vec3(0.0);
    }

    vec3 lightDir = toLight / distanceToLight;
    float influence = max(1.0 - distanceToLight / radius, 0.0);
    float attenuation = influence * influence / (1.0 + distanceToLight * distanceToLight * 0.08);
    float ndotl = max(dot(normal, lightDir), 0.0);
    float shininess = mix(128.0, 8.0, roughness);
    float specular = pow(max(dot(normal, normalize(lightDir + viewDir)), 0.0), shininess);

    vec3 lightColor = light.colorIntensity.rgb * light.colorIntensity.w * attenuation;
    vec3 specColor = mix(vec3(0.08), albedo, metallic);

    return (albedo * ndotl + specColor * specular * (0.35 + metallic * 0.8)) * lightColor;
}

void main()
{
    vec4 positionMetallic = texture(uGPositionMetallic, vUv);
    vec4 normalRoughness = texture(uGNormalRoughness, vUv);
    vec4 albedoAo = texture(uGAlbedoAo, vUv);

    if (length(normalRoughness.xyz) < 0.1)
    {
        oSceneColor = vec4(0.0);
        oBrightColor = vec4(0.0);
        return;
    }

    vec3 worldPos = positionMetallic.xyz;
    vec3 normal = normalize(normalRoughness.xyz);
    vec3 albedo = albedoAo.rgb;
    float roughness = normalRoughness.w;
    float metallic = positionMetallic.w;

    vec3 viewDir = normalize(uCameraPos.xyz - worldPos);
    vec3 lightDir = normalize(-uDirLightDirection.xyz);

    float hemiBlend = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 ambient = albedo * mix(vec3(0.08, 0.10, 0.12), vec3(0.16, 0.15, 0.13), hemiBlend);

    float ndotl = max(dot(normal, lightDir), 0.0);
    float shadow = sampleShadow(worldPos, normal);
    float shininess = mix(144.0, 12.0, roughness);
    float dirSpecular = pow(max(dot(normal, normalize(lightDir + viewDir)), 0.0), shininess);
    vec3 dirSpecColor = mix(vec3(0.06), albedo, metallic);

    vec3 directionalLighting =
        (albedo * ndotl + dirSpecColor * dirSpecular * (0.4 + metallic * 0.6)) *
        uDirLightColorIntensity.rgb * uDirLightColorIntensity.w * (1.0 - shadow);

    vec3 color = ambient + directionalLighting;

    for (int lightIndex = 0; lightIndex < uPointLightCount; ++lightIndex)
    {
        color += shadePointLight(
            uPointLights[lightIndex],
            worldPos,
            normal,
            viewDir,
            albedo,
            roughness,
            metallic);
    }

    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    vec3 bright = luminance > 1.1 ? color : color * smoothstep(0.9, 2.1, luminance) * 0.15;

    oSceneColor = vec4(color, 1.0);
    oBrightColor = vec4(bright, 1.0);
}
)";

constexpr const char* kBlurFragmentShader = R"(#version 460 core
out vec4 fragColor;

in vec2 vUv;

uniform sampler2D uImage;
uniform bool uHorizontal;

void main()
{
    const float weights[5] = float[5](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec2 texelSize = 1.0 / vec2(textureSize(uImage, 0));
    vec2 direction = uHorizontal ? vec2(texelSize.x, 0.0) : vec2(0.0, texelSize.y);

    vec3 result = texture(uImage, vUv).rgb * weights[0];
    for (int index = 1; index < 5; ++index)
    {
        vec2 offset = direction * float(index);
        result += texture(uImage, vUv + offset).rgb * weights[index];
        result += texture(uImage, vUv - offset).rgb * weights[index];
    }

    fragColor = vec4(result, 1.0);
}
)";

constexpr const char* kCompositeFragmentShader = R"(#version 460 core
out vec4 fragColor;

in vec2 vUv;

uniform sampler2D uSceneColor;
uniform sampler2D uBloomColor;
uniform sampler2D uNormalTexture;
uniform sampler2D uShadowMap;
uniform bool uBloomEnabled;
uniform bool uHdrEnabled;
uniform int uDebugMode;

void main()
{
    if (uDebugMode == 1)
    {
        vec3 normal = texture(uNormalTexture, vUv).xyz;
        fragColor = vec4(normal * 0.5 + 0.5, 1.0);
        return;
    }

    if (uDebugMode == 2)
    {
        float depth = texture(uShadowMap, vUv).r;
        float visual = pow(clamp(1.0 - depth, 0.0, 1.0), 0.35);
        fragColor = vec4(vec3(visual), 1.0);
        return;
    }

    vec3 color = texture(uSceneColor, vUv).rgb;
    if (uBloomEnabled)
    {
        color += texture(uBloomColor, vUv).rgb * 0.75;
    }

    if (uHdrEnabled)
    {
        color = vec3(1.0) - exp(-color * 1.05);
    }

    color = pow(max(color, 0.0), vec3(1.0 / 2.2));

    vec2 centeredUv = vUv * 2.0 - 1.0;
    float vignette = smoothstep(1.30, 0.22, length(centeredUv));
    fragColor = vec4(color * vignette, 1.0);
}
)";

class ProceduralDeferredRendererApp
{
public:
    ProceduralDeferredRendererApp() = default;

    ~ProceduralDeferredRendererApp()
    {
        if (resourcesReady_ && window_ != nullptr)
        {
            glfwMakeContextCurrent(window_);
            destroyGlResources();
        }

        if (window_ != nullptr)
        {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }

        glfwTerminate();
    }

    void run()
    {
        initWindowAndContext();
        initRendererResources();
        mainLoop();
    }

private:
    GLFWwindow* window_ = nullptr;
    bool resourcesReady_ = false;

    int framebufferWidth_ = kInitialWindowWidth;
    int framebufferHeight_ = kInitialWindowHeight;

    bool bloomEnabled_ = true;
    bool hdrEnabled_ = true;
    bool animationPaused_ = false;
    DebugView debugView_ = DebugView::Final;

    Camera camera_{};
    bool firstMouseSample_ = true;
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;

    float deltaTime_ = 0.0f;
    float lastFrameTime_ = 0.0f;
    float animationTime_ = 0.0f;
    double titleUpdateAccumulator_ = 0.0;
    int titleFrameCounter_ = 0;

    Program geometryProgram_{};
    Program shadowProgram_{};
    Program lightingProgram_{};
    Program blurProgram_{};
    Program compositeProgram_{};

    GLuint frameUbo_ = 0;
    GLuint lightSsbo_ = 0;
    std::vector<PointLightGpu> pointLights_;

    GLuint checkerTexture_ = 0;
    GLuint stripeTexture_ = 0;
    GLuint noiseTexture_ = 0;

    GLuint gBufferFbo_ = 0;
    GLuint gPositionMetallicTexture_ = 0;
    GLuint gNormalRoughnessTexture_ = 0;
    GLuint gAlbedoAoTexture_ = 0;
    GLuint gDepthRenderbuffer_ = 0;

    GLuint hdrFbo_ = 0;
    GLuint hdrSceneTexture_ = 0;
    GLuint hdrBrightTexture_ = 0;

    std::array<GLuint, 2> pingPongFbos_{0, 0};
    std::array<GLuint, 2> pingPongTextures_{0, 0};
    GLuint blurredBloomTexture_ = 0;

    GLuint shadowFbo_ = 0;
    GLuint shadowDepthTexture_ = 0;

    GLuint fullscreenTriangleVao_ = 0;

    std::vector<Batch> batches_;
    std::vector<CachedPass> renderGraph_;

    static ProceduralDeferredRendererApp& fromWindow(GLFWwindow* window)
    {
        return *static_cast<ProceduralDeferredRendererApp*>(glfwGetWindowUserPointer(window));
    }

    static void framebufferSizeCallback(GLFWwindow* window, int width, int height)
    {
        auto& app = fromWindow(window);
        app.handleResize(width, height);
    }

    static void mousePositionCallback(GLFWwindow* window, double xpos, double ypos)
    {
        auto& app = fromWindow(window);
        app.handleMouseMove(xpos, ypos);
    }

    static void keyCallback(GLFWwindow* window, int key, int, int action, int)
    {
        auto& app = fromWindow(window);
        app.handleKey(key, action);
    }

    void initWindowAndContext()
    {
        if (glfwInit() != GLFW_TRUE)
        {
            fail("Failed to initialize GLFW.");
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_DEPTH_BITS, 24);

        window_ = glfwCreateWindow(
            kInitialWindowWidth,
            kInitialWindowHeight,
            kWindowTitleBase,
            nullptr,
            nullptr);
        if (window_ == nullptr)
        {
            fail("Failed to create a GLFW window with an OpenGL 4.6 core profile.");
        }

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);

        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0)
        {
            fail("Failed to load OpenGL function pointers through GLAD.");
        }

        glfwSetWindowUserPointer(window_, this);
        glfwSetFramebufferSizeCallback(window_, framebufferSizeCallback);
        glfwSetCursorPosCallback(window_, mousePositionCallback);
        glfwSetKeyCallback(window_, keyCallback);
        glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported() == GLFW_TRUE)
        {
            glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }

        glfwGetFramebufferSize(window_, &framebufferWidth_, &framebufferHeight_);
        glViewport(0, 0, framebufferWidth_, framebufferHeight_);

        std::cout << "GPU vendor: " << glGetString(GL_VENDOR) << '\n';
        std::cout << "GPU renderer: " << glGetString(GL_RENDERER) << '\n';
        std::cout << "OpenGL version: " << glGetString(GL_VERSION) << '\n';
        std::cout << "GLSL version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << '\n';

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glDisable(GL_BLEND);
        glClearColor(0.04f, 0.05f, 0.06f, 1.0f);

        lastFrameTime_ = static_cast<float>(glfwGetTime());
    }

    void initRendererResources()
    {
        geometryProgram_ = createProgram(kGeometryVertexShader, kGeometryFragmentShader, "geometry");
        shadowProgram_ = createProgram(kShadowVertexShader, kShadowFragmentShader, "shadow");
        lightingProgram_ = createProgram(kFullscreenTriangleVertexShader, kLightingFragmentShader, "lighting");
        blurProgram_ = createProgram(kFullscreenTriangleVertexShader, kBlurFragmentShader, "blur");
        compositeProgram_ = createProgram(kFullscreenTriangleVertexShader, kCompositeFragmentShader, "composite");

        createFullscreenTriangle();
        createFrameDataUbo();
        createLightSsbo();
        createProceduralTextures();
        createShadowFramebuffer();
        buildScene();
        recreateScreenSizedFramebuffers(framebufferWidth_, framebufferHeight_);
        configurePrograms();
        buildRenderGraph();

        resourcesReady_ = true;
    }

    void createFullscreenTriangle()
    {
        glGenVertexArrays(1, &fullscreenTriangleVao_);
    }

    void createFrameDataUbo()
    {
        glGenBuffers(1, &frameUbo_);
        glBindBuffer(GL_UNIFORM_BUFFER, frameUbo_);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(FrameDataGpu), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameUbo_);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    void createLightSsbo()
    {
        pointLights_.resize(kPointLightCount);
        glGenBuffers(1, &lightSsbo_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightSsbo_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(pointLights_.size() * sizeof(PointLightGpu)), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, lightSsbo_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    GLuint createTextureFromPixels(const std::vector<std::uint8_t>& pixels, int width, int height)
    {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        return texture;
    }

    void createProceduralTextures()
    {
        {
            constexpr int width = 256;
            constexpr int height = 256;
            std::vector<std::uint8_t> pixels(width * height * 4);

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const bool even = ((x / 16) + (y / 16)) % 2 == 0;
                    const glm::vec3 color = even ? glm::vec3(0.92f, 0.90f, 0.86f) : glm::vec3(0.24f, 0.25f, 0.30f);
                    const int index = (y * width + x) * 4;
                    pixels[index + 0] = static_cast<std::uint8_t>(color.r * 255.0f);
                    pixels[index + 1] = static_cast<std::uint8_t>(color.g * 255.0f);
                    pixels[index + 2] = static_cast<std::uint8_t>(color.b * 255.0f);
                    pixels[index + 3] = 255;
                }
            }

            checkerTexture_ = createTextureFromPixels(pixels, width, height);
        }

        {
            constexpr int width = 256;
            constexpr int height = 256;
            std::vector<std::uint8_t> pixels(width * height * 4);

            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const float xf = static_cast<float>(x) / static_cast<float>(width - 1);
                    const float yf = static_cast<float>(y) / static_cast<float>(height - 1);
                    const float stripes = 0.5f + 0.5f * std::sin(xf * glm::two_pi<float>() * 12.0f);
                    const glm::vec3 warm(0.95f, 0.67f, 0.22f);
                    const glm::vec3 cool(0.18f, 0.42f, 0.72f);
                    const glm::vec3 color = glm::mix(cool, warm, stripes) * (0.65f + 0.35f * yf);
                    const int index = (y * width + x) * 4;
                    pixels[index + 0] = static_cast<std::uint8_t>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
                    pixels[index + 1] = static_cast<std::uint8_t>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
                    pixels[index + 2] = static_cast<std::uint8_t>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
                    pixels[index + 3] = 255;
                }
            }

            stripeTexture_ = createTextureFromPixels(pixels, width, height);
        }

        {
            constexpr int width = 128;
            constexpr int height = 128;
            std::mt19937 rng(9001);
            std::uniform_real_distribution<float> distribution(0.0f, 1.0f);

            std::vector<std::uint8_t> pixels(width * height * 4);
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const float noise = distribution(rng);
                    const float pulse = 0.5f + 0.5f * std::sin((static_cast<float>(x + y) * 0.17f) + noise * 6.0f);
                    const glm::vec3 base = glm::mix(glm::vec3(0.18f, 0.21f, 0.24f), glm::vec3(0.86f, 0.90f, 0.94f), pulse);
                    const glm::vec3 color = glm::mix(base, glm::vec3(noise, 0.5f + 0.5f * noise, 1.0f - noise), 0.35f);
                    const int index = (y * width + x) * 4;
                    pixels[index + 0] = static_cast<std::uint8_t>(glm::clamp(color.r, 0.0f, 1.0f) * 255.0f);
                    pixels[index + 1] = static_cast<std::uint8_t>(glm::clamp(color.g, 0.0f, 1.0f) * 255.0f);
                    pixels[index + 2] = static_cast<std::uint8_t>(glm::clamp(color.b, 0.0f, 1.0f) * 255.0f);
                    pixels[index + 3] = 255;
                }
            }

            noiseTexture_ = createTextureFromPixels(pixels, width, height);
        }
    }

    void createShadowFramebuffer()
    {
        glGenFramebuffers(1, &shadowFbo_);
        glGenTextures(1, &shadowDepthTexture_);

        glBindTexture(GL_TEXTURE_2D, shadowDepthTexture_);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_DEPTH_COMPONENT32F,
            kShadowMapSize,
            kShadowMapSize,
            0,
            GL_DEPTH_COMPONENT,
            GL_FLOAT,
            nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
        const float borderColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowDepthTexture_, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        checkFramebufferComplete("shadow");

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    Mesh createMesh(const std::vector<Vertex>& vertices, const std::vector<std::uint32_t>& indices)
    {
        Mesh mesh{};
        mesh.indexCount = static_cast<GLsizei>(indices.size());

        glGenVertexArrays(1, &mesh.vao);
        glGenBuffers(1, &mesh.vbo);
        glGenBuffers(1, &mesh.ebo);

        glBindVertexArray(mesh.vao);

        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)),
            vertices.data(),
            GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
        glBufferData(
            GL_ELEMENT_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
            indices.data(),
            GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));

        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, normal)));

        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, uv)));

        glBindVertexArray(0);

        return mesh;
    }

    Mesh createCubeMesh()
    {
        const std::vector<Vertex> vertices = {
            {{-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f}},
            {{ 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 0.0f}},
            {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f}},
            {{-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f}},

            {{ 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f}},
            {{-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 0.0f}},
            {{-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f}},
            {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f}},

            {{-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
            {{-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}},
            {{-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
            {{-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}},

            {{ 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
            {{ 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}},
            {{ 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
            {{ 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}},

            {{-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f}},
            {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 0.0f}},
            {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f}},
            {{-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 1.0f}},

            {{-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f}},
            {{ 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 0.0f}},
            {{ 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f}},
            {{-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 1.0f}},
        };

        const std::vector<std::uint32_t> indices = {
            0, 1, 2, 0, 2, 3,
            4, 5, 6, 4, 6, 7,
            8, 9, 10, 8, 10, 11,
            12, 13, 14, 12, 14, 15,
            16, 17, 18, 16, 18, 19,
            20, 21, 22, 20, 22, 23,
        };

        return createMesh(vertices, indices);
    }

    Mesh createPlaneMesh()
    {
        const std::vector<Vertex> vertices = {
            {{-0.5f, 0.0f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
            {{ 0.5f, 0.0f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
            {{ 0.5f, 0.0f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
            {{-0.5f, 0.0f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
        };

        const std::vector<std::uint32_t> indices = {
            0, 1, 2,
            0, 2, 3,
        };

        return createMesh(vertices, indices);
    }

    Mesh createSphereMesh(int stacks, int slices)
    {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;

        vertices.reserve(static_cast<std::size_t>((stacks + 1) * (slices + 1)));
        indices.reserve(static_cast<std::size_t>(stacks * slices * 6));

        for (int stack = 0; stack <= stacks; ++stack)
        {
            const float v = static_cast<float>(stack) / static_cast<float>(stacks);
            const float phi = v * glm::pi<float>();
            const float y = std::cos(phi);
            const float ringRadius = std::sin(phi);

            for (int slice = 0; slice <= slices; ++slice)
            {
                const float u = static_cast<float>(slice) / static_cast<float>(slices);
                const float theta = u * glm::two_pi<float>();

                glm::vec3 normal(
                    ringRadius * std::cos(theta),
                    y,
                    ringRadius * std::sin(theta));
                normal = glm::normalize(normal);

                vertices.push_back({
                    normal * 0.5f,
                    normal,
                    glm::vec2(u, 1.0f - v),
                });
            }
        }

        const int stride = slices + 1;
        for (int stack = 0; stack < stacks; ++stack)
        {
            for (int slice = 0; slice < slices; ++slice)
            {
                const std::uint32_t current = static_cast<std::uint32_t>(stack * stride + slice);
                const std::uint32_t next = static_cast<std::uint32_t>((stack + 1) * stride + slice);

                indices.push_back(current);
                indices.push_back(next);
                indices.push_back(current + 1);

                indices.push_back(current + 1);
                indices.push_back(next);
                indices.push_back(next + 1);
            }
        }

        return createMesh(vertices, indices);
    }

    void attachInstanceBuffer(Batch& batch)
    {
        glGenBuffers(1, &batch.instanceBuffer);
        glBindVertexArray(batch.mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, batch.instanceBuffer);
        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(batch.instances.size() * sizeof(InstanceData)),
            batch.instances.data(),
            GL_STATIC_DRAW);

        constexpr GLuint modelLocation = 3;
        for (int column = 0; column < 4; ++column)
        {
            const std::size_t offset = offsetof(InstanceData, model) + sizeof(glm::vec4) * static_cast<std::size_t>(column);
            glEnableVertexAttribArray(modelLocation + static_cast<GLuint>(column));
            glVertexAttribPointer(
                modelLocation + static_cast<GLuint>(column),
                4,
                GL_FLOAT,
                GL_FALSE,
                sizeof(InstanceData),
                reinterpret_cast<void*>(offset));
            glVertexAttribDivisor(modelLocation + static_cast<GLuint>(column), 1);
        }

        glEnableVertexAttribArray(7);
        glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, sizeof(InstanceData), reinterpret_cast<void*>(offsetof(InstanceData, baseColor)));
        glVertexAttribDivisor(7, 1);

        glEnableVertexAttribArray(8);
        glVertexAttribPointer(8, 4, GL_FLOAT, GL_FALSE, sizeof(InstanceData), reinterpret_cast<void*>(offsetof(InstanceData, material)));
        glVertexAttribDivisor(8, 1);

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
    }

    void buildScene()
    {
        batches_.clear();
        batches_.reserve(3);

        batches_.push_back(Batch{"Cubes", createCubeMesh(), 0, {}});
        batches_.push_back(Batch{"Spheres", createSphereMesh(28, 42), 0, {}});
        batches_.push_back(Batch{"Floor", createPlaneMesh(), 0, {}});

        auto& cubeBatch = batches_[0];
        auto& sphereBatch = batches_[1];
        auto& floorBatch = batches_[2];

        {
            InstanceData floor{};
            floor.model = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.02f, 0.0f)), glm::vec3(62.0f, 1.0f, 62.0f));
            floor.baseColor = glm::vec4(0.95f, 0.93f, 0.89f, 1.0f);
            floor.material = glm::vec4(0.0f, 0.92f, 0.02f, 26.0f);
            floorBatch.instances.push_back(floor);
        }

        std::mt19937 rng(1337);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        constexpr int gridWidth = 12;
        constexpr int gridDepth = 12;
        constexpr float spacing = 3.35f;

        for (int z = 0; z < gridDepth; ++z)
        {
            for (int x = 0; x < gridWidth; ++x)
            {
                const float px = (static_cast<float>(x) - (static_cast<float>(gridWidth) - 1.0f) * 0.5f) * spacing;
                const float pz = (static_cast<float>(z) - (static_cast<float>(gridDepth) - 1.0f) * 0.5f) * spacing;

                const float roughness = 0.15f + unit(rng) * 0.78f;
                const float metallic = ((x + z) % 5 == 0) ? (0.45f + unit(rng) * 0.4f) : (unit(rng) * 0.25f);
                const float uvScale = 0.9f + unit(rng) * 2.8f;
                const int materialId = (x * 13 + z * 7) % 3;

                const float hue = std::fmod(static_cast<float>(x) * 0.083f + static_cast<float>(z) * 0.109f, 1.0f);
                const glm::vec3 color = hsvToRgb(hue, 0.50f + unit(rng) * 0.25f, 0.58f + unit(rng) * 0.34f);

                if (((x + z) & 1) == 0)
                {
                    const float baseScale = 0.72f + unit(rng) * 0.42f;
                    const float columnHeight = 0.9f + unit(rng) * 2.7f;
                    const glm::vec3 axis = glm::normalize(glm::vec3(0.25f + unit(rng), 1.0f, 0.25f + unit(rng)));
                    const float angle = unit(rng) * glm::two_pi<float>();

                    InstanceData instance{};
                    instance.model = glm::translate(glm::mat4(1.0f), glm::vec3(px, columnHeight * 0.5f, pz));
                    instance.model = glm::rotate(instance.model, angle, axis);
                    instance.model = glm::scale(instance.model, glm::vec3(baseScale, columnHeight, baseScale));
                    instance.baseColor = glm::vec4(color, 1.0f);
                    instance.material = glm::vec4(static_cast<float>(materialId), roughness, metallic, uvScale);
                    cubeBatch.instances.push_back(instance);
                }
                else
                {
                    const float radius = 0.70f + unit(rng) * 0.55f;
                    const float lift = 0.03f + 0.18f * std::sin((static_cast<float>(x + z) * 0.4f));

                    InstanceData instance{};
                    instance.model = glm::translate(glm::mat4(1.0f), glm::vec3(px, radius + lift, pz));
                    instance.model = glm::scale(instance.model, glm::vec3(radius));
                    instance.baseColor = glm::vec4(color, 1.0f);
                    instance.material = glm::vec4(static_cast<float>((materialId + 1) % 3), roughness * 0.85f, metallic * 0.6f, uvScale * 0.75f);
                    sphereBatch.instances.push_back(instance);
                }
            }
        }

        for (auto& batch : batches_)
        {
            attachInstanceBuffer(batch);
        }
    }

    void destroyScreenSizedFramebuffers()
    {
        deleteFramebuffer(gBufferFbo_);
        deleteTexture(gPositionMetallicTexture_);
        deleteTexture(gNormalRoughnessTexture_);
        deleteTexture(gAlbedoAoTexture_);
        deleteRenderbuffer(gDepthRenderbuffer_);

        deleteFramebuffer(hdrFbo_);
        deleteTexture(hdrSceneTexture_);
        deleteTexture(hdrBrightTexture_);

        for (GLuint& fbo : pingPongFbos_)
        {
            deleteFramebuffer(fbo);
        }

        for (GLuint& texture : pingPongTextures_)
        {
            deleteTexture(texture);
        }

        blurredBloomTexture_ = 0;
    }

    GLuint createScreenTexture(
        GLenum internalFormat,
        GLenum format,
        GLenum type,
        GLint minFilter,
        GLint magFilter) const
    {
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            internalFormat,
            framebufferWidth_,
            framebufferHeight_,
            0,
            format,
            type,
            nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        return texture;
    }

    void recreateScreenSizedFramebuffers(int width, int height)
    {
        framebufferWidth_ = std::max(width, 1);
        framebufferHeight_ = std::max(height, 1);

        destroyScreenSizedFramebuffers();

        glGenFramebuffers(1, &gBufferFbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, gBufferFbo_);

        gPositionMetallicTexture_ = createScreenTexture(GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_NEAREST, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gPositionMetallicTexture_, 0);

        gNormalRoughnessTexture_ = createScreenTexture(GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_NEAREST, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, gNormalRoughnessTexture_, 0);

        gAlbedoAoTexture_ = createScreenTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, gAlbedoAoTexture_, 0);

        glGenRenderbuffers(1, &gDepthRenderbuffer_);
        glBindRenderbuffer(GL_RENDERBUFFER, gDepthRenderbuffer_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, framebufferWidth_, framebufferHeight_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, gDepthRenderbuffer_);

        const GLenum gBufferAttachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
        glDrawBuffers(3, gBufferAttachments);
        checkFramebufferComplete("g-buffer");

        glGenFramebuffers(1, &hdrFbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, hdrFbo_);

        hdrSceneTexture_ = createScreenTexture(GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_LINEAR, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrSceneTexture_, 0);

        hdrBrightTexture_ = createScreenTexture(GL_RGBA16F, GL_RGBA, GL_FLOAT, GL_LINEAR, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, hdrBrightTexture_, 0);

        const GLenum hdrAttachments[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(2, hdrAttachments);
        checkFramebufferComplete("hdr");

        glGenFramebuffers(2, pingPongFbos_.data());
        glGenTextures(2, pingPongTextures_.data());
        for (int index = 0; index < 2; ++index)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, pingPongFbos_[index]);
            glBindTexture(GL_TEXTURE_2D, pingPongTextures_[index]);
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA16F,
                framebufferWidth_,
                framebufferHeight_,
                0,
                GL_RGBA,
                GL_FLOAT,
                nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, pingPongTextures_[index], 0);
            checkFramebufferComplete(index == 0 ? "ping-pong-0" : "ping-pong-1");
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void configurePrograms()
    {
        geometryProgram_.use();
        geometryProgram_.setInt("uCheckerTex", 0);
        geometryProgram_.setInt("uStripeTex", 1);
        geometryProgram_.setInt("uNoiseTex", 2);

        lightingProgram_.use();
        lightingProgram_.setInt("uGPositionMetallic", 0);
        lightingProgram_.setInt("uGNormalRoughness", 1);
        lightingProgram_.setInt("uGAlbedoAo", 2);
        lightingProgram_.setInt("uShadowMap", 3);
        lightingProgram_.setInt("uPointLightCount", kPointLightCount);

        blurProgram_.use();
        blurProgram_.setInt("uImage", 0);

        compositeProgram_.use();
        compositeProgram_.setInt("uSceneColor", 0);
        compositeProgram_.setInt("uBloomColor", 1);
        compositeProgram_.setInt("uNormalTexture", 2);
        compositeProgram_.setInt("uShadowMap", 3);
    }

    void buildRenderGraph()
    {
        renderGraph_.clear();
        renderGraph_.push_back({"Shadow Pass", "Shadow Map", [this]() { shadowPass(); }});
        renderGraph_.push_back({"Geometry Pass", "G-Buffer", [this]() { geometryPass(); }});
        renderGraph_.push_back({"Lighting Pass", "HDR Buffer", [this]() { lightingPass(); }});
        renderGraph_.push_back({"Bloom Blur Pass", "Ping-Pong", [this]() { bloomBlurPass(); }});
        renderGraph_.push_back({"Composite Pass", "Backbuffer", [this]() { compositePass(); }});
    }

    void handleResize(int width, int height)
    {
        if (width <= 0 || height <= 0)
        {
            framebufferWidth_ = std::max(width, 0);
            framebufferHeight_ = std::max(height, 0);
            return;
        }

        if (!resourcesReady_)
        {
            framebufferWidth_ = width;
            framebufferHeight_ = height;
            return;
        }

        recreateScreenSizedFramebuffers(width, height);
        glViewport(0, 0, framebufferWidth_, framebufferHeight_);
    }

    void handleMouseMove(double xpos, double ypos)
    {
        if (firstMouseSample_)
        {
            lastMouseX_ = xpos;
            lastMouseY_ = ypos;
            firstMouseSample_ = false;
            return;
        }

        const float sensitivity = 0.075f;
        const float deltaX = static_cast<float>(xpos - lastMouseX_);
        const float deltaY = static_cast<float>(lastMouseY_ - ypos);

        lastMouseX_ = xpos;
        lastMouseY_ = ypos;

        camera_.yaw += deltaX * sensitivity;
        camera_.pitch = std::clamp(camera_.pitch + deltaY * sensitivity, -89.0f, 89.0f);
    }

    void handleKey(int key, int action)
    {
        if (action != GLFW_PRESS)
        {
            return;
        }

        switch (key)
        {
        case GLFW_KEY_ESCAPE:
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
            break;
        case GLFW_KEY_B:
            bloomEnabled_ = !bloomEnabled_;
            break;
        case GLFW_KEY_H:
            hdrEnabled_ = !hdrEnabled_;
            break;
        case GLFW_KEY_P:
            animationPaused_ = !animationPaused_;
            break;
        case GLFW_KEY_1:
            debugView_ = DebugView::Final;
            break;
        case GLFW_KEY_2:
            debugView_ = DebugView::Normals;
            break;
        case GLFW_KEY_3:
            debugView_ = DebugView::ShadowMap;
            break;
        default:
            break;
        }
    }

    glm::vec3 directionalLightDirection() const
    {
        const float angle = 0.35f + animationTime_ * 0.18f;
        return glm::normalize(glm::vec3(
            -0.64f + 0.18f * std::sin(angle),
            -1.0f,
            -0.42f + 0.22f * std::cos(angle * 1.37f)));
    }

    glm::mat4 projectionMatrix() const
    {
        const float aspect = static_cast<float>(framebufferWidth_) / static_cast<float>(std::max(framebufferHeight_, 1));
        return glm::perspective(glm::radians(camera_.verticalFovDegrees), aspect, 0.1f, 120.0f);
    }

    glm::mat4 lightViewProjectionMatrix() const
    {
        const glm::vec3 center(0.0f, 4.0f, 0.0f);
        const glm::vec3 direction = directionalLightDirection();
        const glm::vec3 lightPosition = center - direction * 30.0f;
        const glm::mat4 lightView = glm::lookAt(lightPosition, center, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 lightProjection = glm::ortho(-34.0f, 34.0f, -34.0f, 34.0f, 1.0f, 80.0f);
        return lightProjection * lightView;
    }

    void processMovementInput()
    {
        const glm::vec3 up(0.0f, 1.0f, 0.0f);
        glm::vec3 forward = camera_.front();
        forward.y = 0.0f;
        if (glm::dot(forward, forward) < 0.0001f)
        {
            forward = glm::vec3(0.0f, 0.0f, -1.0f);
        }
        forward = glm::normalize(forward);

        const glm::vec3 right = glm::normalize(glm::cross(forward, up));
        const float boost = glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 2.4f : 1.0f;
        const float speed = deltaTime_ * 8.0f * boost;

        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS)
        {
            camera_.position += forward * speed;
        }
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS)
        {
            camera_.position -= forward * speed;
        }
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS)
        {
            camera_.position -= right * speed;
        }
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS)
        {
            camera_.position += right * speed;
        }
        if (glfwGetKey(window_, GLFW_KEY_E) == GLFW_PRESS)
        {
            camera_.position += up * speed;
        }
        if (glfwGetKey(window_, GLFW_KEY_Q) == GLFW_PRESS)
        {
            camera_.position -= up * speed;
        }
    }

    void updateLights()
    {
        for (int lightIndex = 0; lightIndex < kPointLightCount; ++lightIndex)
        {
            const float normalized = static_cast<float>(lightIndex) / static_cast<float>(kPointLightCount);
            const float angle = animationTime_ * 0.42f + normalized * glm::two_pi<float>();
            const float ringRadius = 9.0f + 2.0f * std::sin(normalized * glm::two_pi<float>() * 2.0f);
            const float y = 1.9f + 1.4f * std::sin(animationTime_ * 0.73f + normalized * 8.0f);

            const glm::vec3 position(
                std::cos(angle) * ringRadius,
                y,
                std::sin(angle) * ringRadius);
            const glm::vec3 color = hsvToRgb(normalized, 0.78f, 1.0f);

            pointLights_[lightIndex].positionRadius = glm::vec4(position, 10.5f);
            pointLights_[lightIndex].colorIntensity = glm::vec4(color, 8.8f);
        }

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, lightSsbo_);
        glBufferSubData(
            GL_SHADER_STORAGE_BUFFER,
            0,
            static_cast<GLsizeiptr>(pointLights_.size() * sizeof(PointLightGpu)),
            pointLights_.data());
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    }

    void updateFrameData()
    {
        FrameDataGpu frameData{};
        frameData.view = camera_.viewMatrix();
        frameData.proj = projectionMatrix();
        frameData.lightViewProj = lightViewProjectionMatrix();
        frameData.cameraPos = glm::vec4(camera_.position, 1.0f);
        frameData.dirLightDirection = glm::vec4(directionalLightDirection(), 0.0f);
        frameData.dirLightColorIntensity = glm::vec4(1.0f, 0.95f, 0.90f, 1.65f);
        frameData.screenData = glm::vec4(
            static_cast<float>(framebufferWidth_),
            static_cast<float>(framebufferHeight_),
            deltaTime_,
            animationTime_);
        frameData.featureFlags = glm::vec4(
            bloomEnabled_ ? 1.0f : 0.0f,
            hdrEnabled_ ? 1.0f : 0.0f,
            static_cast<float>(static_cast<int>(debugView_)),
            animationPaused_ ? 1.0f : 0.0f);

        glBindBuffer(GL_UNIFORM_BUFFER, frameUbo_);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(FrameDataGpu), &frameData);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }

    void drawScene() const
    {
        for (const auto& batch : batches_)
        {
            glBindVertexArray(batch.mesh.vao);
            glDrawElementsInstanced(
                GL_TRIANGLES,
                batch.mesh.indexCount,
                GL_UNSIGNED_INT,
                nullptr,
                static_cast<GLsizei>(batch.instances.size()));
        }

        glBindVertexArray(0);
    }

    void shadowPass()
    {
        glViewport(0, 0, kShadowMapSize, kShadowMapSize);
        glBindFramebuffer(GL_FRAMEBUFFER, shadowFbo_);
        glClear(GL_DEPTH_BUFFER_BIT);

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(2.0f, 4.0f);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        shadowProgram_.use();
        drawScene();

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    void geometryPass()
    {
        glViewport(0, 0, framebufferWidth_, framebufferHeight_);
        glBindFramebuffer(GL_FRAMEBUFFER, gBufferFbo_);

        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        glClearBufferfv(GL_COLOR, 0, zero);
        glClearBufferfv(GL_COLOR, 1, zero);
        glClearBufferfv(GL_COLOR, 2, zero);
        glClear(GL_DEPTH_BUFFER_BIT);

        glEnable(GL_DEPTH_TEST);

        geometryProgram_.use();
        glBindTextureUnit(0, checkerTexture_);
        glBindTextureUnit(1, stripeTexture_);
        glBindTextureUnit(2, noiseTexture_);
        drawScene();
    }

    void lightingPass()
    {
        glViewport(0, 0, framebufferWidth_, framebufferHeight_);
        glBindFramebuffer(GL_FRAMEBUFFER, hdrFbo_);

        const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        glClearBufferfv(GL_COLOR, 0, clearColor);
        glClearBufferfv(GL_COLOR, 1, clearColor);

        glDisable(GL_DEPTH_TEST);

        lightingProgram_.use();
        glBindTextureUnit(0, gPositionMetallicTexture_);
        glBindTextureUnit(1, gNormalRoughnessTexture_);
        glBindTextureUnit(2, gAlbedoAoTexture_);
        glBindTextureUnit(3, shadowDepthTexture_);

        glBindVertexArray(fullscreenTriangleVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glEnable(GL_DEPTH_TEST);
    }

    void bloomBlurPass()
    {
        glDisable(GL_DEPTH_TEST);

        GLuint sourceTexture = hdrBrightTexture_;
        for (int passIndex = 0; passIndex < kBloomBlurPassCount; ++passIndex)
        {
            const int targetIndex = passIndex % 2;
            glViewport(0, 0, framebufferWidth_, framebufferHeight_);
            glBindFramebuffer(GL_FRAMEBUFFER, pingPongFbos_[targetIndex]);

            blurProgram_.use();
            blurProgram_.setBool("uHorizontal", (passIndex % 2) == 0);
            glBindTextureUnit(0, sourceTexture);

            glBindVertexArray(fullscreenTriangleVao_);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            sourceTexture = pingPongTextures_[targetIndex];
        }

        blurredBloomTexture_ = sourceTexture;
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
    }

    void compositePass()
    {
        glViewport(0, 0, framebufferWidth_, framebufferHeight_);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_DEPTH_TEST);

        compositeProgram_.use();
        compositeProgram_.setBool("uBloomEnabled", bloomEnabled_);
        compositeProgram_.setBool("uHdrEnabled", hdrEnabled_);
        compositeProgram_.setInt("uDebugMode", static_cast<int>(debugView_));

        glBindTextureUnit(0, hdrSceneTexture_);
        glBindTextureUnit(1, blurredBloomTexture_ != 0 ? blurredBloomTexture_ : hdrBrightTexture_);
        glBindTextureUnit(2, gNormalRoughnessTexture_);
        glBindTextureUnit(3, shadowDepthTexture_);

        glBindVertexArray(fullscreenTriangleVao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);

        glEnable(GL_DEPTH_TEST);
    }

    void replayRenderGraph()
    {
        for (const auto& pass : renderGraph_)
        {
            (void)pass.name;
            (void)pass.target;
            pass.execute();
        }
    }

    [[nodiscard]] std::string_view debugViewLabel() const
    {
        switch (debugView_)
        {
        case DebugView::Final:
            return "Final";
        case DebugView::Normals:
            return "Normals";
        case DebugView::ShadowMap:
            return "ShadowMap";
        }

        return "Unknown";
    }

    void updateWindowTitle()
    {
        titleUpdateAccumulator_ += deltaTime_;
        ++titleFrameCounter_;

        if (titleUpdateAccumulator_ < 0.25)
        {
            return;
        }

        const double fps = static_cast<double>(titleFrameCounter_) / titleUpdateAccumulator_;
        const double frameTimeMs = 1000.0 / std::max(fps, 0.0001);

        std::ostringstream stream;
        stream.setf(std::ios::fixed);
        stream.precision(1);
        stream
            << kWindowTitleBase
            << " | " << fps << " FPS"
            << " / " << frameTimeMs << " ms"
            << " | Mode: " << debugViewLabel()
            << " | Bloom: " << (bloomEnabled_ ? "On" : "Off")
            << " | HDR: " << (hdrEnabled_ ? "On" : "Off")
            << " | Anim: " << (animationPaused_ ? "Paused" : "Running");

        glfwSetWindowTitle(window_, stream.str().c_str());
        titleUpdateAccumulator_ = 0.0;
        titleFrameCounter_ = 0;
    }

    void mainLoop()
    {
        while (glfwWindowShouldClose(window_) == GLFW_FALSE)
        {
            glfwPollEvents();

            const float currentTime = static_cast<float>(glfwGetTime());
            deltaTime_ = currentTime - lastFrameTime_;
            lastFrameTime_ = currentTime;

            processMovementInput();
            if (!animationPaused_)
            {
                animationTime_ += deltaTime_;
            }

            updateLights();
            updateFrameData();

            if (framebufferWidth_ > 0 && framebufferHeight_ > 0)
            {
                replayRenderGraph();
                glfwSwapBuffers(window_);
            }

            updateWindowTitle();
        }
    }

    void destroyGlResources()
    {
        destroyScreenSizedFramebuffers();

        deleteFramebuffer(shadowFbo_);
        deleteTexture(shadowDepthTexture_);

        deleteTexture(checkerTexture_);
        deleteTexture(stripeTexture_);
        deleteTexture(noiseTexture_);

        deleteBuffer(frameUbo_);
        deleteBuffer(lightSsbo_);

        deleteVertexArray(fullscreenTriangleVao_);

        for (auto& batch : batches_)
        {
            deleteBuffer(batch.instanceBuffer);
            deleteVertexArray(batch.mesh.vao);
            deleteBuffer(batch.mesh.vbo);
            deleteBuffer(batch.mesh.ebo);
        }
        batches_.clear();

        deleteProgram(geometryProgram_.id);
        deleteProgram(shadowProgram_.id);
        deleteProgram(lightingProgram_.id);
        deleteProgram(blurProgram_.id);
        deleteProgram(compositeProgram_.id);

        resourcesReady_ = false;
    }
};
} // namespace

int main()
{
    try
    {
        ProceduralDeferredRendererApp app;
        app.run();
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return 1;
    }
}
