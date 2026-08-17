#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace
{
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr char kWindowTitle[] = "OpenGL CachedCubePipelines Test";
constexpr GLuint kFrameBlockBinding = 0;

constexpr std::uint32_t kDepthTestBit = 1u << 0;
constexpr std::uint32_t kCullFaceBit = 1u << 1;
constexpr std::uint32_t kWireframeBit = 1u << 2;

enum class PipelineId : std::uint8_t
{
    Lit = 0,
    Wire = 1
};

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
};

struct MeshResources
{
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
};

struct PipelineResources
{
    GLuint program = 0;
    GLint modelLocation = -1;
    GLint baseColorLocation = -1;
};

struct Material
{
    glm::vec4 baseColor{1.0f};
};

struct TransformSeed
{
    glm::vec3 basePosition{0.0f};
    glm::vec3 rotationAxis{0.0f, 1.0f, 0.0f};
    float rotationSpeed = 0.0f;
    float phase = 0.0f;
    float bobAmplitude = 0.0f;
    glm::vec3 scale{1.0f};
};

struct DrawCommand
{
    PipelineId pipeline = PipelineId::Lit;
    GLuint program = 0;
    GLuint vao = 0;
    GLenum drawMode = GL_TRIANGLES;
    GLsizei indexCount = 0;
    GLenum indexType = GL_UNSIGNED_INT;
    std::uint32_t transformIndex = 0;
    std::uint32_t materialIndex = 0;
    std::uint32_t stateBits = 0;
};

struct SceneData
{
    std::vector<Material> materials;
    std::vector<TransformSeed> transforms;
    std::vector<DrawCommand> commands;
};

struct alignas(16) FrameUniforms
{
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::vec4 cameraPosition{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 lightPosition{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec4 timeData{0.0f};
};

static_assert(sizeof(FrameUniforms) % 16 == 0, "Frame uniforms must be 16-byte aligned.");

struct OrbitCamera
{
    glm::vec3 target{0.0f, 0.0f, 0.0f};
    float azimuth = 0.85f;
    float elevation = 0.55f;
    float distance = 20.0f;

    [[nodiscard]] glm::vec3 Position() const
    {
        const float planarDistance = std::cos(elevation) * distance;
        return target + glm::vec3(
            std::cos(azimuth) * planarDistance,
            std::sin(elevation) * distance,
            std::sin(azimuth) * planarDistance);
    }

    [[nodiscard]] glm::mat4 ViewMatrix() const
    {
        return glm::lookAt(Position(), target, glm::vec3(0.0f, 1.0f, 0.0f));
    }
};

struct ToggleLatch
{
    bool previous = false;

    bool Consume(GLFWwindow* window, const int key)
    {
        const bool current = glfwGetKey(window, key) == GLFW_PRESS;
        const bool fired = current && !previous;
        previous = current;
        return fired;
    }
};

struct ReplayStateCache
{
    GLuint program = std::numeric_limits<GLuint>::max();
    GLuint vao = std::numeric_limits<GLuint>::max();
    std::uint32_t stateBits = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t materialIndex = std::numeric_limits<std::uint32_t>::max();
};

struct GLFWScope
{
    GLFWScope()
    {
        glfwSetErrorCallback(GLFWErrorCallback);
        if (glfwInit() != GLFW_TRUE)
        {
            throw std::runtime_error("Failed to initialize GLFW.");
        }
    }

    GLFWScope(const GLFWScope&) = delete;
    GLFWScope& operator=(const GLFWScope&) = delete;

    ~GLFWScope()
    {
        glfwTerminate();
    }

    static void GLFWErrorCallback(const int errorCode, const char* description)
    {
        std::cerr << "GLFW error [" << errorCode << "]: "
                  << (description != nullptr ? description : "Unknown error")
                  << '\n';
    }
};

struct WindowDeleter
{
    void operator()(GLFWwindow* window) const
    {
        if (window != nullptr)
        {
            glfwDestroyWindow(window);
        }
    }
};

using UniqueWindow = std::unique_ptr<GLFWwindow, WindowDeleter>;

const char* kCommonVertexShader = R"(#version 460 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;

layout(std140, binding = 0) uniform FrameBlock
{
    mat4 uView;
    mat4 uProjection;
    vec4 uCameraPos;
    vec4 uLightPos;
    vec4 uTimeData;
};

uniform mat4 uModel;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;

void main()
{
    vec4 worldPosition = uModel * vec4(aPosition, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(uModel)));

    vWorldPos = worldPosition.xyz;
    vNormal = normalize(normalMatrix * aNormal);
    vColor = aColor;

    gl_Position = uProjection * uView * worldPosition;
}
)";

const char* kLitFragmentShader = R"(#version 460 core
layout(std140, binding = 0) uniform FrameBlock
{
    mat4 uView;
    mat4 uProjection;
    vec4 uCameraPos;
    vec4 uLightPos;
    vec4 uTimeData;
};

uniform vec4 uBaseColor;

in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;

out vec4 FragColor;

void main()
{
    vec3 normal = normalize(vNormal);
    vec3 lightDir = normalize(uLightPos.xyz - vWorldPos);
    vec3 viewDir = normalize(uCameraPos.xyz - vWorldPos);
    vec3 halfDir = normalize(lightDir + viewDir);

    float diffuse = max(dot(normal, lightDir), 0.0);
    float specular = pow(max(dot(normal, halfDir), 0.0), 48.0);

    vec3 base = mix(vColor, uBaseColor.rgb, 0.55);
    vec3 ambient = base * 0.18;
    vec3 lit = ambient + base * diffuse * 1.05 + vec3(0.9) * specular * 0.45;

    FragColor = vec4(lit, 1.0);
}
)";

const char* kWireFragmentShader = R"(#version 460 core
layout(std140, binding = 0) uniform FrameBlock
{
    mat4 uView;
    mat4 uProjection;
    vec4 uCameraPos;
    vec4 uLightPos;
    vec4 uTimeData;
};

uniform vec4 uBaseColor;

in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;

out vec4 FragColor;

void main()
{
    vec3 normal = normalize(vNormal);
    vec3 viewDir = normalize(uCameraPos.xyz - vWorldPos);
    float fresnel = pow(1.0 - max(dot(normal, viewDir), 0.0), 2.5);
    float pulse = 0.55 + 0.45 * sin(uTimeData.x * 1.5 + dot(vWorldPos, vec3(0.2, 0.3, 0.25)));
    vec3 tint = mix(uBaseColor.rgb, abs(normal), 0.35);
    vec3 color = tint * (0.7 + 0.3 * pulse) + vec3(0.2, 0.28, 0.45) * fresnel;

    FragColor = vec4(color, 1.0);
}
)";

[[nodiscard]] std::string ShaderInfoLog(const GLuint shader)
{
    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1)
    {
        return {};
    }

    std::string log(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    glGetShaderInfoLog(shader, length, &written, log.data());
    if (written > 0)
    {
        log.resize(static_cast<std::size_t>(written));
    }
    return log;
}

[[nodiscard]] std::string ProgramInfoLog(const GLuint program)
{
    GLint length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    if (length <= 1)
    {
        return {};
    }

    std::string log(static_cast<std::size_t>(length), '\0');
    GLsizei written = 0;
    glGetProgramInfoLog(program, length, &written, log.data());
    if (written > 0)
    {
        log.resize(static_cast<std::size_t>(written));
    }
    return log;
}

[[nodiscard]] GLuint CompileShader(const GLenum shaderType, const char* source)
{
    const GLuint shader = glCreateShader(shaderType);
    if (shader == 0)
    {
        throw std::runtime_error("Failed to create shader object.");
    }

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compileStatus = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
    if (compileStatus != GL_TRUE)
    {
        const std::string log = ShaderInfoLog(shader);
        glDeleteShader(shader);
        throw std::runtime_error("Shader compilation failed: " + log);
    }

    return shader;
}

[[nodiscard]] PipelineResources CreatePipeline(const char* vertexShaderSource, const char* fragmentShaderSource)
{
    const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, vertexShaderSource);
    const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

    const GLuint program = glCreateProgram();
    if (program == 0)
    {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        throw std::runtime_error("Failed to create shader program.");
    }

    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linkStatus = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    if (linkStatus != GL_TRUE)
    {
        const std::string log = ProgramInfoLog(program);
        glDeleteProgram(program);
        throw std::runtime_error("Program link failed: " + log);
    }

    PipelineResources pipeline{};
    pipeline.program = program;
    pipeline.modelLocation = glGetUniformLocation(program, "uModel");
    pipeline.baseColorLocation = glGetUniformLocation(program, "uBaseColor");

    if (pipeline.modelLocation < 0 || pipeline.baseColorLocation < 0)
    {
        glDeleteProgram(program);
        throw std::runtime_error("Required uniform location was not found.");
    }

    return pipeline;
}

void DestroyPipeline(PipelineResources& pipeline)
{
    if (pipeline.program != 0)
    {
        glDeleteProgram(pipeline.program);
    }
    pipeline = {};
}

void AppendFace(
    std::vector<Vertex>& vertices,
    std::vector<std::uint32_t>& indices,
    const glm::vec3& center,
    const glm::vec3& axisU,
    const glm::vec3& axisV,
    const glm::vec3& normal,
    const glm::vec3& color)
{
    const std::uint32_t firstIndex = static_cast<std::uint32_t>(vertices.size());

    vertices.push_back(Vertex{center - axisU - axisV, normal, color});
    vertices.push_back(Vertex{center + axisU - axisV, normal, color});
    vertices.push_back(Vertex{center + axisU + axisV, normal, color});
    vertices.push_back(Vertex{center - axisU + axisV, normal, color});

    indices.push_back(firstIndex + 0u);
    indices.push_back(firstIndex + 1u);
    indices.push_back(firstIndex + 2u);
    indices.push_back(firstIndex + 0u);
    indices.push_back(firstIndex + 2u);
    indices.push_back(firstIndex + 3u);
}

[[nodiscard]] MeshResources CreateCubeMesh()
{
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(24);
    indices.reserve(36);

    const float h = 0.5f;

    AppendFace(vertices, indices, glm::vec3(+h, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -h), glm::vec3(0.0f, +h, 0.0f), glm::vec3(+1.0f, 0.0f, 0.0f), glm::vec3(0.95f, 0.42f, 0.28f));
    AppendFace(vertices, indices, glm::vec3(-h, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, +h), glm::vec3(0.0f, +h, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.24f, 0.72f, 0.92f));
    AppendFace(vertices, indices, glm::vec3(0.0f, +h, 0.0f), glm::vec3(+h, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -h), glm::vec3(0.0f, +1.0f, 0.0f), glm::vec3(0.98f, 0.83f, 0.31f));
    AppendFace(vertices, indices, glm::vec3(0.0f, -h, 0.0f), glm::vec3(+h, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, +h), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.31f, 0.86f, 0.56f));
    AppendFace(vertices, indices, glm::vec3(0.0f, 0.0f, +h), glm::vec3(+h, 0.0f, 0.0f), glm::vec3(0.0f, +h, 0.0f), glm::vec3(0.0f, 0.0f, +1.0f), glm::vec3(0.95f, 0.28f, 0.54f));
    AppendFace(vertices, indices, glm::vec3(0.0f, 0.0f, -h), glm::vec3(-h, 0.0f, 0.0f), glm::vec3(0.0f, +h, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.66f, 0.47f, 0.97f));

    MeshResources mesh{};
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
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, position)));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, normal)));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, color)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    mesh.indexCount = static_cast<GLsizei>(indices.size());
    return mesh;
}

void DestroyMesh(MeshResources& mesh)
{
    if (mesh.ebo != 0)
    {
        glDeleteBuffers(1, &mesh.ebo);
    }
    if (mesh.vbo != 0)
    {
        glDeleteBuffers(1, &mesh.vbo);
    }
    if (mesh.vao != 0)
    {
        glDeleteVertexArrays(1, &mesh.vao);
    }
    mesh = {};
}

[[nodiscard]] std::uint32_t ComposeStateBits(const bool depthTest, const bool cullFace, const bool wireframe)
{
    std::uint32_t bits = 0;
    if (depthTest)
    {
        bits |= kDepthTestBit;
    }
    if (cullFace)
    {
        bits |= kCullFaceBit;
    }
    if (wireframe)
    {
        bits |= kWireframeBit;
    }
    return bits;
}

void ApplyStateBits(const std::uint32_t desiredBits, const std::uint32_t currentBits)
{
    if (((desiredBits ^ currentBits) & kDepthTestBit) != 0u)
    {
        if ((desiredBits & kDepthTestBit) != 0u)
        {
            glEnable(GL_DEPTH_TEST);
        }
        else
        {
            glDisable(GL_DEPTH_TEST);
        }
    }

    if (((desiredBits ^ currentBits) & kCullFaceBit) != 0u)
    {
        if ((desiredBits & kCullFaceBit) != 0u)
        {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        }
        else
        {
            glDisable(GL_CULL_FACE);
        }
    }

    if (((desiredBits ^ currentBits) & kWireframeBit) != 0u)
    {
        glPolygonMode(GL_FRONT_AND_BACK, (desiredBits & kWireframeBit) != 0u ? GL_LINE : GL_FILL);
    }
}

[[nodiscard]] SceneData BuildScene(const MeshResources& cubeMesh, const std::array<PipelineResources, 2>& pipelines)
{
    SceneData scene{};

    const std::array<glm::vec4, 4> litPalette{
        glm::vec4(0.94f, 0.37f, 0.30f, 1.0f),
        glm::vec4(0.97f, 0.77f, 0.24f, 1.0f),
        glm::vec4(0.22f, 0.73f, 0.93f, 1.0f),
        glm::vec4(0.33f, 0.84f, 0.50f, 1.0f)};

    const std::array<glm::vec4, 4> wirePalette{
        glm::vec4(0.16f, 0.91f, 1.00f, 1.0f),
        glm::vec4(1.00f, 0.48f, 0.18f, 1.0f),
        glm::vec4(0.96f, 0.24f, 0.69f, 1.0f),
        glm::vec4(0.74f, 0.94f, 0.31f, 1.0f)};

    scene.materials.reserve(litPalette.size() + wirePalette.size());
    for (const glm::vec4& color : litPalette)
    {
        scene.materials.push_back(Material{color});
    }
    for (const glm::vec4& color : wirePalette)
    {
        scene.materials.push_back(Material{color});
    }

    scene.transforms.reserve(64);
    scene.commands.reserve(64);

    // The draw list is built and sorted once during initialization, then replayed every frame.
    for (int z = 0; z < 4; ++z)
    {
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 4; ++x)
            {
                TransformSeed transform{};
                transform.basePosition = glm::vec3(
                    (static_cast<float>(x) - 1.5f) * 2.8f,
                    (static_cast<float>(y) - 1.5f) * 2.3f,
                    (static_cast<float>(z) - 1.5f) * 2.8f);
                transform.rotationAxis = glm::normalize(glm::vec3(
                    0.35f + 0.17f * static_cast<float>(x),
                    1.00f + 0.11f * static_cast<float>(y),
                    0.45f + 0.13f * static_cast<float>(z)));
                transform.rotationSpeed = 0.35f + 0.08f * static_cast<float>((x + y + z) % 5);
                transform.phase = 0.6f * static_cast<float>((x * 7 + y * 5 + z * 3) % 11);
                transform.bobAmplitude = 0.10f + 0.03f * static_cast<float>((x + 2 * y + z) % 4);
                transform.scale = glm::vec3(1.0f);

                scene.transforms.push_back(transform);
                const std::uint32_t transformIndex = static_cast<std::uint32_t>(scene.transforms.size() - 1u);

                const bool lit = ((x + y + z) & 1) == 0;
                const std::uint32_t materialIndex = lit
                    ? static_cast<std::uint32_t>((x + z) % static_cast<int>(litPalette.size()))
                    : static_cast<std::uint32_t>(litPalette.size() + ((x * 3 + y + z) % static_cast<int>(wirePalette.size())));

                DrawCommand command{};
                command.pipeline = lit ? PipelineId::Lit : PipelineId::Wire;
                command.program = pipelines[lit ? 0u : 1u].program;
                command.vao = cubeMesh.vao;
                command.drawMode = GL_TRIANGLES;
                command.indexCount = cubeMesh.indexCount;
                command.indexType = GL_UNSIGNED_INT;
                command.transformIndex = transformIndex;
                command.materialIndex = materialIndex;
                command.stateBits = lit
                    ? ComposeStateBits(true, true, false)
                    : ComposeStateBits(true, false, true);

                scene.commands.push_back(command);
            }
        }
    }

    std::sort(
        scene.commands.begin(),
        scene.commands.end(),
        [](const DrawCommand& lhs, const DrawCommand& rhs)
        {
            return std::tie(lhs.program, lhs.vao, lhs.stateBits, lhs.materialIndex, lhs.transformIndex) <
                   std::tie(rhs.program, rhs.vao, rhs.stateBits, rhs.materialIndex, rhs.transformIndex);
        });

    return scene;
}

void UpdateCamera(OrbitCamera& camera, GLFWwindow* window, const float deltaTime)
{
    const float orbitSpeed = 1.25f;
    const float zoomSpeed = 8.0f;

    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
    {
        camera.azimuth += orbitSpeed * deltaTime;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
    {
        camera.azimuth -= orbitSpeed * deltaTime;
    }
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
    {
        camera.distance -= zoomSpeed * deltaTime;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
    {
        camera.distance += zoomSpeed * deltaTime;
    }

    camera.distance = std::clamp(camera.distance, 8.0f, 34.0f);
}

void UpdateModelMatrices(
    const std::vector<TransformSeed>& transforms,
    const float animationTime,
    std::vector<glm::mat4>& modelMatrices)
{
    modelMatrices.resize(transforms.size(), glm::mat4(1.0f));

    const glm::mat4 identity(1.0f);
    for (std::size_t index = 0; index < transforms.size(); ++index)
    {
        const TransformSeed& transform = transforms[index];
        const float bob = std::sin(animationTime * 0.9f + transform.phase) * transform.bobAmplitude;

        glm::mat4 model = glm::translate(identity, transform.basePosition + glm::vec3(0.0f, bob, 0.0f));
        model = glm::rotate(model, animationTime * transform.rotationSpeed + transform.phase, transform.rotationAxis);
        model = glm::scale(model, transform.scale);

        modelMatrices[index] = model;
    }
}

void RenderCachedCommands(
    const SceneData& scene,
    const std::array<PipelineResources, 2>& pipelines,
    const std::vector<glm::mat4>& modelMatrices,
    const bool showLit,
    const bool showWire)
{
    ReplayStateCache state{};

    for (const DrawCommand& command : scene.commands)
    {
        if (command.pipeline == PipelineId::Lit && !showLit)
        {
            continue;
        }
        if (command.pipeline == PipelineId::Wire && !showWire)
        {
            continue;
        }

        const PipelineResources& pipeline = pipelines[command.pipeline == PipelineId::Lit ? 0u : 1u];

        if (command.program != state.program)
        {
            glUseProgram(command.program);
            state.program = command.program;
            state.materialIndex = std::numeric_limits<std::uint32_t>::max();
        }

        if (command.vao != state.vao)
        {
            glBindVertexArray(command.vao);
            state.vao = command.vao;
        }

        if (command.stateBits != state.stateBits)
        {
            ApplyStateBits(command.stateBits, state.stateBits);
            state.stateBits = command.stateBits;
        }

        if (command.materialIndex != state.materialIndex)
        {
            glUniform4fv(
                pipeline.baseColorLocation,
                1,
                glm::value_ptr(scene.materials[command.materialIndex].baseColor));
            state.materialIndex = command.materialIndex;
        }

        glUniformMatrix4fv(
            pipeline.modelLocation,
            1,
            GL_FALSE,
            glm::value_ptr(modelMatrices[command.transformIndex]));

        glDrawElements(command.drawMode, command.indexCount, command.indexType, nullptr);
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
}

void ValidateContext()
{
    GLint major = 0;
    GLint minor = 0;
    GLint profileMask = 0;

    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profileMask);

    if (major < 4 || (major == 4 && minor < 6))
    {
        throw std::runtime_error("OpenGL 4.6 core profile context was not created.");
    }

    if ((profileMask & GL_CONTEXT_CORE_PROFILE_BIT) == 0)
    {
        throw std::runtime_error("A core profile OpenGL context was not created.");
    }
}

void PrintOpenGLInfo()
{
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* shadingLanguage = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));

    std::cout << "OpenGL version: " << (version != nullptr ? version : "Unknown") << '\n';
    std::cout << "OpenGL vendor: " << (vendor != nullptr ? vendor : "Unknown") << '\n';
    std::cout << "OpenGL renderer: " << (renderer != nullptr ? renderer : "Unknown") << '\n';
    std::cout << "GLSL version: " << (shadingLanguage != nullptr ? shadingLanguage : "Unknown") << '\n';
}
} // namespace

int main()
{
    try
    {
        GLFWScope glfwScope;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

        UniqueWindow window(glfwCreateWindow(kWindowWidth, kWindowHeight, kWindowTitle, nullptr, nullptr));
        if (!window)
        {
            throw std::runtime_error("Failed to create a GLFW window with OpenGL 4.6 core profile.");
        }

        glfwMakeContextCurrent(window.get());
        glfwSwapInterval(1);

        if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0)
        {
            throw std::runtime_error("Failed to load OpenGL functions through GLAD.");
        }

        ValidateContext();
        PrintOpenGLInfo();
        std::cout << "Controls: Esc close, W/S zoom, A/D orbit, Space pause, 1 toggle lit cubes, 2 toggle wire cubes.\n";

        std::array<PipelineResources, 2> pipelines{
            CreatePipeline(kCommonVertexShader, kLitFragmentShader),
            CreatePipeline(kCommonVertexShader, kWireFragmentShader)};

        MeshResources cubeMesh = CreateCubeMesh();

        GLuint frameUbo = 0;
        glGenBuffers(1, &frameUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, frameUbo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(FrameUniforms), nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, kFrameBlockBinding, frameUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

        SceneData scene = BuildScene(cubeMesh, pipelines);
        std::cout << "Cube count: " << scene.transforms.size() << '\n';
        std::cout << "Cached draw commands: " << scene.commands.size() << '\n';

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glClearColor(0.07f, 0.09f, 0.13f, 1.0f);

        OrbitCamera camera{};
        ToggleLatch pauseToggle{};
        ToggleLatch litToggle{};
        ToggleLatch wireToggle{};

        bool paused = false;
        bool showLit = true;
        bool showWire = true;
        float animationTime = 0.0f;
        float lastFrameTime = static_cast<float>(glfwGetTime());
        int viewportWidth = 0;
        int viewportHeight = 0;
        std::vector<glm::mat4> modelMatrices(scene.transforms.size(), glm::mat4(1.0f));

        while (glfwWindowShouldClose(window.get()) == 0)
        {
            glfwPollEvents();

            if (glfwGetKey(window.get(), GLFW_KEY_ESCAPE) == GLFW_PRESS)
            {
                glfwSetWindowShouldClose(window.get(), GLFW_TRUE);
            }

            const float currentFrameTime = static_cast<float>(glfwGetTime());
            float deltaTime = std::min(currentFrameTime - lastFrameTime, 0.1f);
            if (deltaTime < 0.0f)
            {
                deltaTime = 0.0f;
            }
            lastFrameTime = currentFrameTime;

            if (pauseToggle.Consume(window.get(), GLFW_KEY_SPACE))
            {
                paused = !paused;
                std::cout << "Animation " << (paused ? "paused" : "running") << '\n';
            }
            if (litToggle.Consume(window.get(), GLFW_KEY_1))
            {
                showLit = !showLit;
                std::cout << "Pipeline 1 (lit) " << (showLit ? "enabled" : "disabled") << '\n';
            }
            if (wireToggle.Consume(window.get(), GLFW_KEY_2))
            {
                showWire = !showWire;
                std::cout << "Pipeline 2 (wire) " << (showWire ? "enabled" : "disabled") << '\n';
            }

            UpdateCamera(camera, window.get(), deltaTime);

            if (!paused)
            {
                animationTime += deltaTime;
            }

            int framebufferWidth = 0;
            int framebufferHeight = 0;
            glfwGetFramebufferSize(window.get(), &framebufferWidth, &framebufferHeight);
            if (framebufferWidth == 0 || framebufferHeight == 0)
            {
                glfwWaitEventsTimeout(0.05);
                continue;
            }

            if (framebufferWidth != viewportWidth || framebufferHeight != viewportHeight)
            {
                glViewport(0, 0, framebufferWidth, framebufferHeight);
                viewportWidth = framebufferWidth;
                viewportHeight = framebufferHeight;
            }

            UpdateModelMatrices(scene.transforms, animationTime, modelMatrices);

            const glm::vec3 cameraPosition = camera.Position();
            const glm::vec3 lightPosition(
                std::cos(animationTime * 0.55f) * 12.0f,
                8.0f + std::sin(animationTime * 0.40f) * 2.0f,
                std::sin(animationTime * 0.55f) * 12.0f);

            // A single UBO feeds view/projection/camera/light data to both pipelines each frame.
            FrameUniforms frameUniforms{};
            frameUniforms.view = camera.ViewMatrix();
            frameUniforms.projection = glm::perspective(
                glm::radians(60.0f),
                static_cast<float>(framebufferWidth) / static_cast<float>(framebufferHeight),
                0.1f,
                100.0f);
            frameUniforms.cameraPosition = glm::vec4(cameraPosition, 1.0f);
            frameUniforms.lightPosition = glm::vec4(lightPosition, 1.0f);
            frameUniforms.timeData = glm::vec4(animationTime, deltaTime, paused ? 1.0f : 0.0f, 0.0f);

            glBindBuffer(GL_UNIFORM_BUFFER, frameUbo);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(FrameUniforms), &frameUniforms);
            glBindBuffer(GL_UNIFORM_BUFFER, 0);

            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            RenderCachedCommands(scene, pipelines, modelMatrices, showLit, showWire);
            glfwSwapBuffers(window.get());
        }

        glDeleteBuffers(1, &frameUbo);
        DestroyMesh(cubeMesh);
        for (PipelineResources& pipeline : pipelines)
        {
            DestroyPipeline(pipeline);
        }

        return EXIT_SUCCESS;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Fatal error: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
