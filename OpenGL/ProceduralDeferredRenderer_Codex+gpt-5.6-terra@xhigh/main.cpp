// ProceduralDeferredRenderer: all geometry, texture pixels and shaders are generated in this file.
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
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kInitialWidth = 1600;
constexpr int kInitialHeight = 900;
constexpr int kShadowSize = 2048;
constexpr int kLightCount = 20;
constexpr int kBlurPasses = 10;

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

// This layout is consumed directly by the instanced geometry shader attributes.
struct InstanceGpu {
    glm::mat4 model { 1.0f };
    glm::vec4 albedoMetal { 1.0f };
};

// std140, binding point 0. Matrices and vectors are naturally 16-byte aligned.
struct FrameGpu {
    glm::mat4 view { 1.0f };
    glm::mat4 projection { 1.0f };
    glm::mat4 viewProjection { 1.0f };
    glm::mat4 lightSpace { 1.0f };
    glm::vec4 cameraTime { 0.0f };
    glm::vec4 sunDirectionIntensity { 0.0f, -1.0f, 0.0f, 3.0f };
    glm::vec4 renderSize { 0.0f };
};

// std430, binding point 1.
struct LightGpu {
    glm::vec4 positionRadius { 0.0f };
    glm::vec4 colorIntensity { 1.0f };
};

struct Mesh {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
};

struct InstanceBatch {
    GLuint buffer = 0;
    GLsizei count = 0;
};

enum class PassKind { Shadow, Geometry, Lighting, BloomBlur, Present };

// The pass list is created once. Every frame only changes data buffers and replays it.
struct PassCommand {
    PassKind kind;
    const char* name;
    GLuint framebuffer;
    GLuint program;
    GLbitfield clearMask;
};

class Renderer;
Renderer* gRenderer = nullptr;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

GLuint compileShader(GLenum type, const char* source, const char* label) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok == GL_TRUE) return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    glDeleteShader(shader);
    fail(std::string("Shader compilation failed (") + label + "):\n" + log);
}

GLuint makeProgram(const char* vertex, const char* fragment, const char* label) {
    const GLuint vs = compileShader(GL_VERTEX_SHADER, vertex, label);
    const GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragment, label);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok == GL_TRUE) return program;
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
    glGetProgramInfoLog(program, length, nullptr, log.data());
    glDeleteProgram(program);
    fail(std::string("Program link failed (") + label + "):\n" + log);
}

void requireFramebufferComplete(GLuint fbo, const char* label) {
    const GLenum state = glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    if (state != GL_FRAMEBUFFER_COMPLETE) {
        std::ostringstream stream;
        stream << label << " framebuffer is incomplete (0x" << std::hex << state << ')';
        fail(stream.str());
    }
}

GLuint createColorTexture(GLenum format, int width, int height, GLenum minFilter = GL_LINEAR) {
    GLuint texture = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture);
    glTextureStorage2D(texture, 1, format, width, height);
    glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, minFilter);
    glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(texture, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

const char* kGeometryVertex = R"GLSL(
#version 460 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
layout (location = 3) in mat4 aModel;
layout (location = 7) in vec4 aAlbedoMetal;

layout (std140, binding = 0) uniform FrameData {
    mat4 uView;
    mat4 uProjection;
    mat4 uViewProjection;
    mat4 uLightSpace;
    vec4 uCameraTime;
    vec4 uSunDirectionIntensity;
    vec4 uRenderSize;
};

out VS_OUT {
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 uv;
    vec4 albedoMetal;
} vsOut;

void main() {
    vec4 world = aModel * vec4(aPosition, 1.0);
    vsOut.worldPosition = world.xyz;
    vsOut.worldNormal = normalize(mat3(transpose(inverse(aModel))) * aNormal);
    vsOut.uv = aUv;
    vsOut.albedoMetal = aAlbedoMetal;
    gl_Position = uViewProjection * world;
}
)GLSL";

const char* kGeometryFragment = R"GLSL(
#version 460 core
layout (location = 0) out vec4 gPositionRoughness;
layout (location = 1) out vec4 gNormalMetal;
layout (location = 2) out vec4 gAlbedoAo;

in VS_OUT {
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 uv;
    vec4 albedoMetal;
} fsIn;

layout (binding = 0) uniform sampler2D uCheckerTexture;

void main() {
    // A generated checker texture breaks up the otherwise solid procedural materials.
    vec3 checker = texture(uCheckerTexture, fsIn.uv * 2.0).rgb;
    vec3 albedo = fsIn.albedoMetal.rgb * mix(vec3(0.62), checker, 0.38);
    float roughness = 0.3 + 0.55 * fract(fsIn.uv.x * 3.1 + fsIn.uv.y * 1.7);
    gPositionRoughness = vec4(fsIn.worldPosition, roughness);
    gNormalMetal = vec4(normalize(fsIn.worldNormal), fsIn.albedoMetal.a);
    gAlbedoAo = vec4(albedo, 1.0);
}
)GLSL";

const char* kShadowVertex = R"GLSL(
#version 460 core
layout (location = 0) in vec3 aPosition;
layout (location = 3) in mat4 aModel;
layout (std140, binding = 0) uniform FrameData {
    mat4 uView;
    mat4 uProjection;
    mat4 uViewProjection;
    mat4 uLightSpace;
    vec4 uCameraTime;
    vec4 uSunDirectionIntensity;
    vec4 uRenderSize;
};
void main() { gl_Position = uLightSpace * aModel * vec4(aPosition, 1.0); }
)GLSL";

const char* kShadowFragment = R"GLSL(
#version 460 core
void main() { }
)GLSL";

const char* kFullscreenVertex = R"GLSL(
#version 460 core
out vec2 vUv;
void main() {
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vec2 p = positions[gl_VertexID];
    vUv = p * 0.5 + 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
}
)GLSL";

const char* kLightingFragment = R"GLSL(
#version 460 core
layout (location = 0) out vec4 oScene;
layout (location = 1) out vec4 oBright;
in vec2 vUv;

layout (binding = 1) uniform sampler2D uGPositionRoughness;
layout (binding = 2) uniform sampler2D uGNormalMetal;
layout (binding = 3) uniform sampler2D uGAlbedoAo;
layout (binding = 4) uniform sampler2D uShadowMap;

layout (std140, binding = 0) uniform FrameData {
    mat4 uView;
    mat4 uProjection;
    mat4 uViewProjection;
    mat4 uLightSpace;
    vec4 uCameraTime;
    vec4 uSunDirectionIntensity;
    vec4 uRenderSize;
};

struct PointLight { vec4 positionRadius; vec4 colorIntensity; };
layout (std430, binding = 1) readonly buffer PointLights { PointLight lights[]; };
uniform int uLightCount;

float shadowPcf(vec3 position, vec3 normal) {
    vec4 lightClip = uLightSpace * vec4(position, 1.0);
    vec3 projected = lightClip.xyz / lightClip.w;
    projected = projected * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.z < 0.0 || projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0)
        return 0.0;
    vec3 lightDirection = normalize(-uSunDirectionIntensity.xyz);
    float bias = max(0.0017 * (1.0 - dot(normal, lightDirection)), 0.00035);
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float closest = texture(uShadowMap, projected.xy + vec2(x, y) * texel).r;
            shadow += projected.z - bias > closest ? 1.0 : 0.0;
        }
    return shadow / 9.0;
}

void main() {
    vec4 positionRoughness = texture(uGPositionRoughness, vUv);
    vec4 normalMetal = texture(uGNormalMetal, vUv);
    vec3 position = positionRoughness.xyz;
    vec3 normal = normalize(normalMetal.xyz);
    vec3 albedo = texture(uGAlbedoAo, vUv).rgb;
    float roughness = positionRoughness.a;
    float metallic = normalMetal.a;
    vec3 viewDirection = normalize(uCameraTime.xyz - position);

    // Hemisphere ambient keeps the floor readable where direct light is shadowed.
    vec3 result = albedo * (0.055 + 0.095 * max(normal.y, 0.0));
    vec3 sunDirection = normalize(-uSunDirectionIntensity.xyz);
    float NdotL = max(dot(normal, sunDirection), 0.0);
    vec3 halfDirection = normalize(sunDirection + viewDirection);
    float sunSpecular = pow(max(dot(normal, halfDirection), 0.0), mix(12.0, 84.0, 1.0 - roughness));
    float shadow = shadowPcf(position, normal);
    result += (1.0 - shadow) * uSunDirectionIntensity.w * (albedo * NdotL + vec3(0.28 + metallic * 0.7) * sunSpecular);

    for (int i = 0; i < uLightCount; ++i) {
        vec3 toLight = lights[i].positionRadius.xyz - position;
        float distanceToLight = length(toLight);
        float radius = lights[i].positionRadius.w;
        if (distanceToLight >= radius) continue;
        vec3 lightDirection = toLight / max(distanceToLight, 0.001);
        float diffuse = max(dot(normal, lightDirection), 0.0);
        vec3 pointHalf = normalize(lightDirection + viewDirection);
        float specular = pow(max(dot(normal, pointHalf), 0.0), mix(10.0, 72.0, 1.0 - roughness));
        float falloff = pow(max(1.0 - distanceToLight / radius, 0.0), 2.0);
        vec3 radiance = lights[i].colorIntensity.rgb * lights[i].colorIntensity.a * falloff;
        result += radiance * (albedo * diffuse + vec3(0.16 + metallic * 0.8) * specular);
    }

    oScene = vec4(result, 1.0);
    float brightness = dot(result, vec3(0.2126, 0.7152, 0.0722));
    oBright = brightness > 1.0 ? vec4(result, 1.0) : vec4(0.0);
}
)GLSL";

const char* kBlurFragment = R"GLSL(
#version 460 core
out vec4 oColor;
in vec2 vUv;
layout (binding = 5) uniform sampler2D uInput;
uniform bool uHorizontal;
void main() {
    const float weights[5] = float[5](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec2 texel = 1.0 / vec2(textureSize(uInput, 0));
    vec3 color = texture(uInput, vUv).rgb * weights[0];
    vec2 axis = uHorizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    for (int i = 1; i < 5; ++i) {
        color += texture(uInput, vUv + axis * float(i)).rgb * weights[i];
        color += texture(uInput, vUv - axis * float(i)).rgb * weights[i];
    }
    oColor = vec4(color, 1.0);
}
)GLSL";

const char* kPresentFragment = R"GLSL(
#version 460 core
out vec4 oColor;
in vec2 vUv;
layout (binding = 6) uniform sampler2D uScene;
layout (binding = 7) uniform sampler2D uBloom;
layout (binding = 8) uniform sampler2D uGNormalMetal;
layout (binding = 9) uniform sampler2D uShadowMap;
uniform bool uBloomEnabled;
uniform bool uHdrEnabled;
uniform int uDebugMode;
void main() {
    if (uDebugMode == 2) {
        vec3 normal = texture(uGNormalMetal, vUv).xyz;
        oColor = vec4(normal * 0.5 + 0.5, 1.0);
        return;
    }
    if (uDebugMode == 3) {
        float depth = texture(uShadowMap, vUv).r;
        oColor = vec4(vec3(depth), 1.0);
        return;
    }
    vec3 color = texture(uScene, vUv).rgb;
    if (uBloomEnabled) color += texture(uBloom, vUv).rgb;
    if (uHdrEnabled) color = color / (color + vec3(1.0));
    // sRGB gamma correction is intentionally applied only in the final post pass.
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    oColor = vec4(color, 1.0);
}
)GLSL";

class Renderer {
public:
    explicit Renderer(GLFWwindow* window) : window_(window) { }

    void initialize() {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

        geometryProgram_ = makeProgram(kGeometryVertex, kGeometryFragment, "G-buffer geometry");
        shadowProgram_ = makeProgram(kShadowVertex, kShadowFragment, "shadow depth");
        lightingProgram_ = makeProgram(kFullscreenVertex, kLightingFragment, "deferred lighting");
        blurProgram_ = makeProgram(kFullscreenVertex, kBlurFragment, "Gaussian bloom blur");
        presentProgram_ = makeProgram(kFullscreenVertex, kPresentFragment, "HDR present");

        createMeshes();
        createCheckerTexture();
        createBuffersAndInstances();
        createShadowTarget();
        glfwGetFramebufferSize(window_, &width_, &height_);
        resizeRenderTargets(width_, height_);
        buildRenderGraph();
    }

    void onResize(int width, int height) {
        if (width <= 0 || height <= 0) return;
        resizeRenderTargets(width, height);
        buildRenderGraph();
    }

    void update(float dt, float absoluteTime) {
        updateInput(dt);
        if (!paused_) animationTime_ = absoluteTime;

        const glm::vec3 forward = cameraFront();
        const glm::mat4 view = glm::lookAt(cameraPosition_, cameraPosition_ + forward, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 projection = glm::perspective(glm::radians(60.0f), static_cast<float>(width_) / static_cast<float>(height_), 0.1f, 90.0f);
        const glm::vec3 sunDirection = glm::normalize(glm::vec3(-0.42f, -0.82f, -0.30f));
        const glm::mat4 lightProjection = glm::ortho(-18.0f, 18.0f, -18.0f, 18.0f, 1.0f, 54.0f);
        const glm::mat4 lightView = glm::lookAt(-sunDirection * 25.0f, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

        frame_.view = view;
        frame_.projection = projection;
        frame_.viewProjection = projection * view;
        frame_.lightSpace = lightProjection * lightView;
        frame_.cameraTime = glm::vec4(cameraPosition_, animationTime_);
        frame_.sunDirectionIntensity = glm::vec4(sunDirection, 2.5f);
        frame_.renderSize = glm::vec4(static_cast<float>(width_), static_cast<float>(height_), 1.0f / width_, 1.0f / height_);
        glNamedBufferSubData(frameUbo_, 0, sizeof(FrameGpu), &frame_);

        updateLights();
    }

    void render() {
        for (const PassCommand& command : renderGraph_) {
            switch (command.kind) {
            case PassKind::Shadow: renderShadow(command); break;
            case PassKind::Geometry: renderGeometry(command); break;
            case PassKind::Lighting: renderLighting(command); break;
            case PassKind::BloomBlur: renderBloom(command); break;
            case PassKind::Present: renderPresent(command); break;
            }
        }
    }

    void shutdown() {
        destroyRenderTargets();
        if (shadowFbo_) glDeleteFramebuffers(1, &shadowFbo_);
        if (shadowTexture_) glDeleteTextures(1, &shadowTexture_);
        if (checkerTexture_) glDeleteTextures(1, &checkerTexture_);
        if (frameUbo_) glDeleteBuffers(1, &frameUbo_);
        if (lightSsbo_) glDeleteBuffers(1, &lightSsbo_);
        destroyBatch(cubeInstances_);
        destroyBatch(sphereInstances_);
        destroyBatch(floorInstances_);
        destroyMesh(cube_);
        destroyMesh(plane_);
        destroyMesh(sphere_);
        for (GLuint program : { geometryProgram_, shadowProgram_, lightingProgram_, blurProgram_, presentProgram_ })
            if (program) glDeleteProgram(program);
    }

private:
    GLFWwindow* window_ = nullptr;
    int width_ = kInitialWidth;
    int height_ = kInitialHeight;
    Mesh cube_ {};
    Mesh plane_ {};
    Mesh sphere_ {};
    InstanceBatch cubeInstances_ {};
    InstanceBatch sphereInstances_ {};
    InstanceBatch floorInstances_ {};
    GLuint checkerTexture_ = 0;
    GLuint frameUbo_ = 0;
    GLuint lightSsbo_ = 0;
    GLuint shadowFbo_ = 0;
    GLuint shadowTexture_ = 0;
    GLuint gBufferFbo_ = 0;
    GLuint gPositionRoughness_ = 0;
    GLuint gNormalMetal_ = 0;
    GLuint gAlbedoAo_ = 0;
    GLuint gDepth_ = 0;
    GLuint lightingFbo_ = 0;
    GLuint hdrScene_ = 0;
    GLuint hdrBright_ = 0;
    std::array<GLuint, 2> bloomFbo_ {};
    std::array<GLuint, 2> bloomTexture_ {};
    GLuint geometryProgram_ = 0;
    GLuint shadowProgram_ = 0;
    GLuint lightingProgram_ = 0;
    GLuint blurProgram_ = 0;
    GLuint presentProgram_ = 0;
    FrameGpu frame_ {};
    std::array<LightGpu, kLightCount> lights_ {};
    std::vector<PassCommand> renderGraph_;
    glm::vec3 cameraPosition_ { 10.0f, 7.5f, 17.0f };
    float yaw_ = -122.0f;
    float pitch_ = -18.0f;
    float animationTime_ = 0.0f;
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;
    bool firstMouse_ = true;
    bool bloomEnabled_ = true;
    bool hdrEnabled_ = true;
    bool paused_ = false;
    int debugMode_ = 1;
    std::array<bool, GLFW_KEY_LAST + 1> keyWasDown_ {};

    static void destroyMesh(Mesh& mesh) {
        if (mesh.ebo) glDeleteBuffers(1, &mesh.ebo);
        if (mesh.vbo) glDeleteBuffers(1, &mesh.vbo);
        if (mesh.vao) glDeleteVertexArrays(1, &mesh.vao);
        mesh = {};
    }

    static void destroyBatch(InstanceBatch& batch) {
        if (batch.buffer) glDeleteBuffers(1, &batch.buffer);
        batch = {};
    }

    static Mesh createMesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices) {
        Mesh mesh;
        mesh.indexCount = static_cast<GLsizei>(indices.size());
        glCreateVertexArrays(1, &mesh.vao);
        glCreateBuffers(1, &mesh.vbo);
        glCreateBuffers(1, &mesh.ebo);
        glNamedBufferData(mesh.vbo, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STATIC_DRAW);
        glNamedBufferData(mesh.ebo, static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)), indices.data(), GL_STATIC_DRAW);
        glVertexArrayVertexBuffer(mesh.vao, 0, mesh.vbo, 0, sizeof(Vertex));
        glVertexArrayElementBuffer(mesh.vao, mesh.ebo);
        glEnableVertexArrayAttrib(mesh.vao, 0);
        glEnableVertexArrayAttrib(mesh.vao, 1);
        glEnableVertexArrayAttrib(mesh.vao, 2);
        glVertexArrayAttribFormat(mesh.vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
        glVertexArrayAttribFormat(mesh.vao, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
        glVertexArrayAttribFormat(mesh.vao, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
        glVertexArrayAttribBinding(mesh.vao, 0, 0);
        glVertexArrayAttribBinding(mesh.vao, 1, 0);
        glVertexArrayAttribBinding(mesh.vao, 2, 0);
        return mesh;
    }

    static void attachInstances(Mesh& mesh, GLuint instanceBuffer) {
        glVertexArrayVertexBuffer(mesh.vao, 1, instanceBuffer, 0, sizeof(InstanceGpu));
        for (GLuint col = 0; col < 4; ++col) {
            const GLuint location = 3 + col;
            glEnableVertexArrayAttrib(mesh.vao, location);
            glVertexArrayAttribFormat(mesh.vao, location, 4, GL_FLOAT, GL_FALSE, static_cast<GLuint>(sizeof(glm::vec4) * col));
            glVertexArrayAttribBinding(mesh.vao, location, 1);
            glVertexArrayBindingDivisor(mesh.vao, 1, 1);
        }
        glEnableVertexArrayAttrib(mesh.vao, 7);
        glVertexArrayAttribFormat(mesh.vao, 7, 4, GL_FLOAT, GL_FALSE, offsetof(InstanceGpu, albedoMetal));
        glVertexArrayAttribBinding(mesh.vao, 7, 1);
    }

    void createMeshes() {
        // Indexed cube: six independent faces supply correct normals and UVs.
        const std::array<Vertex, 24> cubeVertices = {{
            {{-0.5f,-0.5f, 0.5f},{ 0, 0, 1},{0,0}}, {{ 0.5f,-0.5f, 0.5f},{ 0, 0, 1},{1,0}}, {{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1},{1,1}}, {{-0.5f, 0.5f, 0.5f},{ 0, 0, 1},{0,1}},
            {{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1},{0,0}}, {{-0.5f,-0.5f,-0.5f},{ 0, 0,-1},{1,0}}, {{-0.5f, 0.5f,-0.5f},{ 0, 0,-1},{1,1}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 0,-1},{0,1}},
            {{-0.5f,-0.5f,-0.5f},{-1, 0, 0},{0,0}}, {{-0.5f,-0.5f, 0.5f},{-1, 0, 0},{1,0}}, {{-0.5f, 0.5f, 0.5f},{-1, 0, 0},{1,1}}, {{-0.5f, 0.5f,-0.5f},{-1, 0, 0},{0,1}},
            {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0},{0,0}}, {{ 0.5f,-0.5f,-0.5f},{ 1, 0, 0},{1,0}}, {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0},{1,1}}, {{ 0.5f, 0.5f, 0.5f},{ 1, 0, 0},{0,1}},
            {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0},{0,0}}, {{ 0.5f, 0.5f, 0.5f},{ 0, 1, 0},{1,0}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0},{1,1}}, {{-0.5f, 0.5f,-0.5f},{ 0, 1, 0},{0,1}},
            {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0},{0,0}}, {{ 0.5f,-0.5f,-0.5f},{ 0,-1, 0},{1,0}}, {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0},{1,1}}, {{-0.5f,-0.5f, 0.5f},{ 0,-1, 0},{0,1}}
        }};
        const std::vector<unsigned int> cubeIndices = {
            0,1,2, 2,3,0, 4,5,6, 6,7,4, 8,9,10, 10,11,8,
            12,13,14, 14,15,12, 16,17,18, 18,19,16, 20,21,22, 22,23,20 };
        cube_ = createMesh(std::vector<Vertex>(cubeVertices.begin(), cubeVertices.end()), cubeIndices);

        const std::vector<Vertex> planeVertices = {
            {{-18,0,-18},{0,1,0},{0,0}}, {{18,0,-18},{0,1,0},{9,0}},
            {{18,0,18},{0,1,0},{9,9}}, {{-18,0,18},{0,1,0},{0,9}} };
        plane_ = createMesh(planeVertices, { 0, 2, 1, 2, 0, 3 });

        // UV sphere: a non-cube indexed mesh generated entirely in memory.
        constexpr int sectors = 36;
        constexpr int stacks = 24;
        std::vector<Vertex> sphereVertices;
        std::vector<unsigned int> sphereIndices;
        sphereVertices.reserve((sectors + 1) * (stacks + 1));
        for (int y = 0; y <= stacks; ++y) {
            const float v = static_cast<float>(y) / stacks;
            const float phi = v * glm::pi<float>();
            for (int x = 0; x <= sectors; ++x) {
                const float u = static_cast<float>(x) / sectors;
                const float theta = u * glm::two_pi<float>();
                const glm::vec3 normal { std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta) };
                sphereVertices.push_back({ normal, normal, { u, v } });
            }
        }
        for (int y = 0; y < stacks; ++y) for (int x = 0; x < sectors; ++x) {
            const unsigned int a = y * (sectors + 1) + x;
            const unsigned int b = a + sectors + 1;
            sphereIndices.insert(sphereIndices.end(), { a, a + 1, b, a + 1, b + 1, b });
        }
        sphere_ = createMesh(sphereVertices, sphereIndices);
    }

    void createCheckerTexture() {
        constexpr int size = 128;
        std::array<unsigned char, size * size * 4> pixels {};
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            const bool dark = ((x / 16) + (y / 16)) % 2 == 0;
            const int index = (y * size + x) * 4;
            pixels[index + 0] = dark ? 58 : 210;
            pixels[index + 1] = dark ? 72 : 185;
            pixels[index + 2] = dark ? 94 : 142;
            pixels[index + 3] = 255;
        }
        glCreateTextures(GL_TEXTURE_2D, 1, &checkerTexture_);
        glTextureStorage2D(checkerTexture_, 1, GL_RGBA8, size, size);
        glTextureSubImage2D(checkerTexture_, 0, 0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTextureParameteri(checkerTexture_, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(checkerTexture_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(checkerTexture_, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(checkerTexture_, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glGenerateTextureMipmap(checkerTexture_);
    }

    InstanceBatch createBatch(const std::vector<InstanceGpu>& instances) {
        InstanceBatch batch;
        batch.count = static_cast<GLsizei>(instances.size());
        glCreateBuffers(1, &batch.buffer);
        glNamedBufferData(batch.buffer, static_cast<GLsizeiptr>(instances.size() * sizeof(InstanceGpu)), instances.data(), GL_STATIC_DRAW);
        return batch;
    }

    void createBuffersAndInstances() {
        std::mt19937 random(0xC0FFEEu); // fixed seed makes the scene deterministic.
        std::uniform_real_distribution<float> colorJitter(0.78f, 1.15f);
        std::vector<InstanceGpu> cubes;
        cubes.reserve(169);
        for (int z = -6; z <= 6; ++z) for (int x = -6; x <= 6; ++x) {
            const float h = 0.55f + 1.25f * std::abs(std::sin(static_cast<float>(x * 11 + z * 7) * 0.31f));
            InstanceGpu instance;
            instance.model = glm::translate(glm::mat4(1.0f), glm::vec3(x * 1.75f, h * 0.5f, z * 1.75f));
            instance.model = glm::rotate(instance.model, 0.19f * static_cast<float>((x * 5 + z * 3) % 8), glm::vec3(0.0f, 1.0f, 0.0f));
            instance.model = glm::scale(instance.model, glm::vec3(0.70f, h, 0.70f));
            const glm::vec3 palette = (x + z) % 3 == 0 ? glm::vec3(0.23f, 0.65f, 0.96f) : ((x - z) % 3 == 0 ? glm::vec3(0.96f, 0.31f, 0.20f) : glm::vec3(0.30f, 0.91f, 0.47f));
            instance.albedoMetal = glm::vec4(palette * colorJitter(random), (x + z) % 5 == 0 ? 0.72f : 0.08f);
            cubes.push_back(instance);
        }
        cubeInstances_ = createBatch(cubes);
        attachInstances(cube_, cubeInstances_.buffer);

        std::vector<InstanceGpu> spheres;
        for (int i = 0; i < 12; ++i) {
            const float angle = glm::two_pi<float>() * i / 12.0f;
            InstanceGpu sphere;
            sphere.model = glm::translate(glm::mat4(1.0f), glm::vec3(std::cos(angle) * 10.4f, 2.15f + 0.4f * std::sin(angle * 3.0f), std::sin(angle) * 10.4f));
            sphere.model = glm::scale(sphere.model, glm::vec3(1.08f));
            sphere.albedoMetal = glm::vec4(0.75f + 0.2f * std::cos(angle), 0.20f + 0.4f * std::sin(angle * 2.0f) + 0.4f, 0.89f, 0.88f);
            spheres.push_back(sphere);
        }
        sphereInstances_ = createBatch(spheres);
        attachInstances(sphere_, sphereInstances_.buffer);

        const InstanceGpu floor { glm::mat4(1.0f), glm::vec4(0.20f, 0.24f, 0.31f, 0.04f) };
        floorInstances_ = createBatch({ floor });
        attachInstances(plane_, floorInstances_.buffer);

        glCreateBuffers(1, &frameUbo_);
        glNamedBufferData(frameUbo_, sizeof(FrameGpu), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, frameUbo_);
        glCreateBuffers(1, &lightSsbo_);
        glNamedBufferData(lightSsbo_, sizeof(lights_), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, lightSsbo_);
    }

    void createShadowTarget() {
        glCreateTextures(GL_TEXTURE_2D, 1, &shadowTexture_);
        glTextureStorage2D(shadowTexture_, 1, GL_DEPTH_COMPONENT32F, kShadowSize, kShadowSize);
        glTextureParameteri(shadowTexture_, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(shadowTexture_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(shadowTexture_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTextureParameteri(shadowTexture_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        const float border[] = { 1.0f, 1.0f, 1.0f, 1.0f };
        glTextureParameterfv(shadowTexture_, GL_TEXTURE_BORDER_COLOR, border);
        glCreateFramebuffers(1, &shadowFbo_);
        glNamedFramebufferTexture(shadowFbo_, GL_DEPTH_ATTACHMENT, shadowTexture_, 0);
        glNamedFramebufferDrawBuffer(shadowFbo_, GL_NONE);
        glNamedFramebufferReadBuffer(shadowFbo_, GL_NONE);
        requireFramebufferComplete(shadowFbo_, "Shadow");
    }

    void resizeRenderTargets(int width, int height) {
        if (width <= 0 || height <= 0) return;
        width_ = width;
        height_ = height;
        destroyRenderTargets();

        glCreateFramebuffers(1, &gBufferFbo_);
        gPositionRoughness_ = createColorTexture(GL_RGBA16F, width_, height_, GL_NEAREST);
        gNormalMetal_ = createColorTexture(GL_RGBA16F, width_, height_, GL_NEAREST);
        gAlbedoAo_ = createColorTexture(GL_RGBA8, width_, height_, GL_NEAREST);
        gDepth_ = createColorTexture(GL_DEPTH_COMPONENT24, width_, height_, GL_NEAREST);
        glNamedFramebufferTexture(gBufferFbo_, GL_COLOR_ATTACHMENT0, gPositionRoughness_, 0);
        glNamedFramebufferTexture(gBufferFbo_, GL_COLOR_ATTACHMENT1, gNormalMetal_, 0);
        glNamedFramebufferTexture(gBufferFbo_, GL_COLOR_ATTACHMENT2, gAlbedoAo_, 0);
        glNamedFramebufferTexture(gBufferFbo_, GL_DEPTH_ATTACHMENT, gDepth_, 0);
        constexpr GLenum gAttachments[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
        glNamedFramebufferDrawBuffers(gBufferFbo_, 3, gAttachments);
        requireFramebufferComplete(gBufferFbo_, "G-buffer");

        glCreateFramebuffers(1, &lightingFbo_);
        hdrScene_ = createColorTexture(GL_RGBA16F, width_, height_);
        hdrBright_ = createColorTexture(GL_RGBA16F, width_, height_);
        glNamedFramebufferTexture(lightingFbo_, GL_COLOR_ATTACHMENT0, hdrScene_, 0);
        glNamedFramebufferTexture(lightingFbo_, GL_COLOR_ATTACHMENT1, hdrBright_, 0);
        constexpr GLenum lightAttachments[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glNamedFramebufferDrawBuffers(lightingFbo_, 2, lightAttachments);
        requireFramebufferComplete(lightingFbo_, "Lighting HDR");

        for (int i = 0; i < 2; ++i) {
            glCreateFramebuffers(1, &bloomFbo_[i]);
            bloomTexture_[i] = createColorTexture(GL_RGBA16F, width_, height_);
            glNamedFramebufferTexture(bloomFbo_[i], GL_COLOR_ATTACHMENT0, bloomTexture_[i], 0);
            glNamedFramebufferDrawBuffer(bloomFbo_[i], GL_COLOR_ATTACHMENT0);
            requireFramebufferComplete(bloomFbo_[i], "Bloom ping-pong");
        }
    }

    void destroyRenderTargets() {
        for (GLuint& fbo : bloomFbo_) if (fbo) glDeleteFramebuffers(1, &fbo);
        for (GLuint& texture : bloomTexture_) if (texture) glDeleteTextures(1, &texture);
        bloomFbo_.fill(0);
        bloomTexture_.fill(0);
        if (lightingFbo_) glDeleteFramebuffers(1, &lightingFbo_);
        if (hdrScene_) glDeleteTextures(1, &hdrScene_);
        if (hdrBright_) glDeleteTextures(1, &hdrBright_);
        lightingFbo_ = hdrScene_ = hdrBright_ = 0;
        if (gBufferFbo_) glDeleteFramebuffers(1, &gBufferFbo_);
        for (GLuint* texture : { &gPositionRoughness_, &gNormalMetal_, &gAlbedoAo_, &gDepth_ })
            if (*texture) glDeleteTextures(1, texture);
        gBufferFbo_ = gPositionRoughness_ = gNormalMetal_ = gAlbedoAo_ = gDepth_ = 0;
    }

    void buildRenderGraph() {
        renderGraph_ = {
            { PassKind::Shadow, "shadow depth", shadowFbo_, shadowProgram_, GL_DEPTH_BUFFER_BIT },
            { PassKind::Geometry, "G-buffer", gBufferFbo_, geometryProgram_, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT },
            { PassKind::Lighting, "deferred HDR lighting", lightingFbo_, lightingProgram_, GL_COLOR_BUFFER_BIT },
            { PassKind::BloomBlur, "ping-pong bloom blur", 0, blurProgram_, GL_COLOR_BUFFER_BIT },
            { PassKind::Present, "tone mapping / debug", 0, presentProgram_, GL_COLOR_BUFFER_BIT }
        };
    }

    void drawScene() const {
        glBindVertexArray(plane_.vao);
        glDrawElementsInstanced(GL_TRIANGLES, plane_.indexCount, GL_UNSIGNED_INT, nullptr, floorInstances_.count);
        glBindVertexArray(cube_.vao);
        glDrawElementsInstanced(GL_TRIANGLES, cube_.indexCount, GL_UNSIGNED_INT, nullptr, cubeInstances_.count);
        glBindVertexArray(sphere_.vao);
        glDrawElementsInstanced(GL_TRIANGLES, sphere_.indexCount, GL_UNSIGNED_INT, nullptr, sphereInstances_.count);
    }

    void renderShadow(const PassCommand& command) {
        glViewport(0, 0, kShadowSize, kShadowSize);
        glBindFramebuffer(GL_FRAMEBUFFER, command.framebuffer);
        glClear(command.clearMask);
        glUseProgram(command.program);
        glCullFace(GL_FRONT); // reduces acne on the large floor.
        drawScene();
        glCullFace(GL_BACK);
    }

    void renderGeometry(const PassCommand& command) {
        glViewport(0, 0, width_, height_);
        glBindFramebuffer(GL_FRAMEBUFFER, command.framebuffer);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(command.clearMask);
        glUseProgram(command.program);
        glBindTextureUnit(0, checkerTexture_);
        drawScene();
    }

    void renderLighting(const PassCommand& command) {
        glViewport(0, 0, width_, height_);
        glBindFramebuffer(GL_FRAMEBUFFER, command.framebuffer);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(command.clearMask);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(command.program);
        glBindTextureUnit(1, gPositionRoughness_);
        glBindTextureUnit(2, gNormalMetal_);
        glBindTextureUnit(3, gAlbedoAo_);
        glBindTextureUnit(4, shadowTexture_);
        glUniform1i(glGetUniformLocation(command.program, "uLightCount"), kLightCount);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_DEPTH_TEST);
    }

    void renderBloom(const PassCommand& command) {
        if (!bloomEnabled_) return;
        glViewport(0, 0, width_, height_);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(command.program);
        bool horizontal = true;
        bool first = true;
        for (int i = 0; i < kBlurPasses; ++i) {
            const int target = horizontal ? 1 : 0;
            glBindFramebuffer(GL_FRAMEBUFFER, bloomFbo_[target]);
            glBindTextureUnit(5, first ? hdrBright_ : bloomTexture_[horizontal ? 0 : 1]);
            glUniform1i(glGetUniformLocation(command.program, "uHorizontal"), horizontal ? GL_TRUE : GL_FALSE);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            horizontal = !horizontal;
            first = false;
        }
        glEnable(GL_DEPTH_TEST);
    }

    void renderPresent(const PassCommand& command) {
        glViewport(0, 0, width_, height_);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.015f, 0.018f, 0.028f, 1.0f);
        glClear(command.clearMask);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(command.program);
        glBindTextureUnit(6, hdrScene_);
        glBindTextureUnit(7, bloomEnabled_ ? bloomTexture_[0] : hdrBright_);
        glBindTextureUnit(8, gNormalMetal_);
        glBindTextureUnit(9, shadowTexture_);
        glUniform1i(glGetUniformLocation(command.program, "uBloomEnabled"), bloomEnabled_ ? GL_TRUE : GL_FALSE);
        glUniform1i(glGetUniformLocation(command.program, "uHdrEnabled"), hdrEnabled_ ? GL_TRUE : GL_FALSE);
        glUniform1i(glGetUniformLocation(command.program, "uDebugMode"), debugMode_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEnable(GL_DEPTH_TEST);
    }

    void updateLights() {
        for (int i = 0; i < kLightCount; ++i) {
            const float phase = glm::two_pi<float>() * i / static_cast<float>(kLightCount);
            const float orbit = phase + animationTime_ * (0.28f + 0.015f * (i % 5));
            lights_[i].positionRadius = glm::vec4(std::cos(orbit) * (6.5f + (i % 4) * 1.1f), 1.8f + 1.4f * std::sin(animationTime_ * 0.9f + phase * 2.0f), std::sin(orbit) * (6.5f + (i % 4) * 1.1f), 5.4f);
            const glm::vec3 color = (i % 3 == 0) ? glm::vec3(1.0f, 0.18f, 0.08f) : (i % 3 == 1 ? glm::vec3(0.08f, 0.45f, 1.0f) : glm::vec3(0.20f, 1.0f, 0.42f));
            lights_[i].colorIntensity = glm::vec4(color, 4.2f + 1.7f * std::sin(animationTime_ + phase));
        }
        glNamedBufferSubData(lightSsbo_, 0, sizeof(lights_), lights_.data());
    }

    glm::vec3 cameraFront() const {
        const float yaw = glm::radians(yaw_);
        const float pitch = glm::radians(pitch_);
        return glm::normalize(glm::vec3(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)));
    }

    bool pressedOnce(int key) {
        const bool down = glfwGetKey(window_, key) == GLFW_PRESS;
        const bool result = down && !keyWasDown_[key];
        keyWasDown_[key] = down;
        return result;
    }

    void updateInput(float dt) {
        if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window_, GLFW_TRUE);
        const float speed = (glfwGetKey(window_, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 9.0f : 4.5f) * dt;
        const glm::vec3 front = cameraFront();
        const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
        if (glfwGetKey(window_, GLFW_KEY_W) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_UP) == GLFW_PRESS) cameraPosition_ += front * speed;
        if (glfwGetKey(window_, GLFW_KEY_S) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_DOWN) == GLFW_PRESS) cameraPosition_ -= front * speed;
        if (glfwGetKey(window_, GLFW_KEY_A) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_LEFT) == GLFW_PRESS) cameraPosition_ -= right * speed;
        if (glfwGetKey(window_, GLFW_KEY_D) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_RIGHT) == GLFW_PRESS) cameraPosition_ += right * speed;
        if (pressedOnce(GLFW_KEY_B)) bloomEnabled_ = !bloomEnabled_;
        if (pressedOnce(GLFW_KEY_H)) hdrEnabled_ = !hdrEnabled_;
        if (pressedOnce(GLFW_KEY_P)) paused_ = !paused_;
        if (pressedOnce(GLFW_KEY_1)) debugMode_ = 1;
        if (pressedOnce(GLFW_KEY_2)) debugMode_ = 2;
        if (pressedOnce(GLFW_KEY_3)) debugMode_ = 3;
    }

public:
    void onMouse(double x, double y) {
        if (firstMouse_) { lastMouseX_ = x; lastMouseY_ = y; firstMouse_ = false; return; }
        constexpr float sensitivity = 0.10f;
        yaw_ += static_cast<float>(x - lastMouseX_) * sensitivity;
        pitch_ -= static_cast<float>(y - lastMouseY_) * sensitivity;
        pitch_ = glm::clamp(pitch_, -89.0f, 89.0f);
        lastMouseX_ = x;
        lastMouseY_ = y;
    }

    std::string title(double fps, double milliseconds) const {
        const char* mode = debugMode_ == 1 ? "Final" : (debugMode_ == 2 ? "Normals" : "Shadow Map");
        std::ostringstream stream;
        stream << "OpenGL ProceduralDeferredRenderer Test | " << std::fixed << std::setprecision(1)
               << fps << " FPS  " << milliseconds << " ms | " << mode
               << " | Bloom " << (bloomEnabled_ ? "on" : "off")
               << " | HDR " << (hdrEnabled_ ? "on" : "off") << (paused_ ? " | PAUSED" : "");
        return stream.str();
    }
};

void framebufferSizeCallback(GLFWwindow*, int width, int height) {
    if (gRenderer) gRenderer->onResize(width, height);
}

void mouseCallback(GLFWwindow*, double x, double y) {
    if (gRenderer) gRenderer->onMouse(x, y);
}

void glfwErrorCallback(int code, const char* description) {
    std::cerr << "GLFW error " << code << ": " << description << '\n';
}

} // namespace

int main() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (glfwInit() != GLFW_TRUE) {
        std::cerr << "Unable to initialize GLFW. This renderer requires an OpenGL 4.6-capable driver.\n";
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(kInitialWidth, kInitialHeight, "OpenGL ProceduralDeferredRenderer Test", nullptr, nullptr);
    if (!window) {
        std::cerr << "Could not create the required OpenGL 4.6 core window. Update the GPU driver or use an OpenGL 4.6 GPU.\n";
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
        std::cerr << "GLAD could not load OpenGL 4.6 entry points.\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    std::cout << "GPU vendor:    " << glGetString(GL_VENDOR) << '\n'
              << "GPU renderer:  " << glGetString(GL_RENDERER) << '\n'
              << "OpenGL:        " << glGetString(GL_VERSION) << '\n'
              << "GLSL:          " << glGetString(GL_SHADING_LANGUAGE_VERSION) << '\n' << std::flush;

    try {
        Renderer renderer(window);
        gRenderer = &renderer;
        glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
        glfwSetCursorPosCallback(window, mouseCallback);
        glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        renderer.initialize();

        double previous = glfwGetTime();
        double titleClock = previous;
        int titleFrames = 0;
        while (!glfwWindowShouldClose(window)) {
            const double now = glfwGetTime();
            const float deltaTime = static_cast<float>(std::min(now - previous, 0.1));
            previous = now;
            glfwPollEvents();
            renderer.update(deltaTime, static_cast<float>(now));
            renderer.render();
            glfwSwapBuffers(window);

            ++titleFrames;
            if (now - titleClock >= 0.35) {
                const double elapsed = now - titleClock;
                const double fps = titleFrames / elapsed;
                glfwSetWindowTitle(window, renderer.title(fps, 1000.0 / fps).c_str());
                titleFrames = 0;
                titleClock = now;
            }
        }
        renderer.shutdown();
        gRenderer = nullptr;
    } catch (const std::exception& error) {
        std::cerr << "Renderer initialization failed: " << error.what() << '\n';
        gRenderer = nullptr;
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
