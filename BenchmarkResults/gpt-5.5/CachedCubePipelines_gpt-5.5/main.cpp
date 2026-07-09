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
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr int kCubeCount = 64;
constexpr int kMaterialCount = 8;

constexpr GLuint kFrameBinding = 0;
constexpr GLuint kTransformBinding = 1;
constexpr GLuint kMaterialBinding = 2;

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec3 color;
};

struct MaterialGpu
{
    glm::vec4 baseColor;
    glm::vec4 params;
};

struct FrameGpu
{
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec4 cameraPosition;
    glm::vec4 lightPosition;
    glm::vec4 timeInfo;
};

enum class PipelineId : std::uint8_t
{
    Lit = 0,
    Wire = 1
};

struct CachedDrawCommand
{
    PipelineId pipeline;
    GLuint program;
    GLuint vao;
    GLsizei indexCount;
    GLuint transformIndex;
    GLuint materialIndex;
    GLenum drawMode;
    bool depthTest;
    bool cullFace;
    bool wireframe;
    bool visible;
};

struct MeshGpu
{
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    GLsizei indexCount = 0;
};

struct Camera
{
    glm::vec3 position = glm::vec3(0.0f, 6.0f, 16.0f);
    float yaw = -90.0f;
    float pitch = -18.0f;
};

struct AppState
{
    bool animate = true;
    bool showLit = true;
    bool showWire = true;
    bool previousSpace = false;
    bool previousOne = false;
    bool previousTwo = false;
    Camera camera;
};

const char* kLitVertexShader = R"GLSL(
#version 460 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec3 aColor;

layout (std140, binding = 0) uniform FrameData
{
    mat4 uView;
    mat4 uProjection;
    vec4 uCameraPosition;
    vec4 uLightPosition;
    vec4 uTimeInfo;
};

layout (std140, binding = 1) uniform TransformData
{
    mat4 uModels[128];
};

uniform int uTransformIndex;

out VS_OUT
{
    vec3 worldPosition;
    vec3 normal;
    vec3 vertexColor;
} vsOut;

void main()
{
    mat4 model = uModels[uTransformIndex];
    vec4 world = model * vec4(aPosition, 1.0);
    vsOut.worldPosition = world.xyz;
    vsOut.normal = mat3(transpose(inverse(model))) * aNormal;
    vsOut.vertexColor = aColor;
    gl_Position = uProjection * uView * world;
}
)GLSL";

const char* kLitFragmentShader = R"GLSL(
#version 460 core
layout (location = 0) out vec4 FragColor;

layout (std140, binding = 0) uniform FrameData
{
    mat4 uView;
    mat4 uProjection;
    vec4 uCameraPosition;
    vec4 uLightPosition;
    vec4 uTimeInfo;
};

struct Material
{
    vec4 baseColor;
    vec4 params;
};

layout (std140, binding = 2) uniform MaterialData
{
    Material uMaterials[8];
};

uniform int uMaterialIndex;

in VS_OUT
{
    vec3 worldPosition;
    vec3 normal;
    vec3 vertexColor;
} fsIn;

void main()
{
    Material material = uMaterials[uMaterialIndex];
    vec3 normal = normalize(fsIn.normal);
    vec3 lightDirection = normalize(uLightPosition.xyz - fsIn.worldPosition);
    vec3 viewDirection = normalize(uCameraPosition.xyz - fsIn.worldPosition);
    vec3 halfVector = normalize(lightDirection + viewDirection);

    float diffuse = max(dot(normal, lightDirection), 0.0);
    float specular = pow(max(dot(normal, halfVector), 0.0), material.params.x) * material.params.y;
    vec3 base = material.baseColor.rgb * fsIn.vertexColor;
    vec3 ambient = base * 0.12;
    vec3 color = ambient + base * diffuse + vec3(specular);
    FragColor = vec4(color, 1.0);
}
)GLSL";

const char* kWireVertexShader = R"GLSL(
#version 460 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec3 aColor;

layout (std140, binding = 0) uniform FrameData
{
    mat4 uView;
    mat4 uProjection;
    vec4 uCameraPosition;
    vec4 uLightPosition;
    vec4 uTimeInfo;
};

layout (std140, binding = 1) uniform TransformData
{
    mat4 uModels[128];
};

uniform int uTransformIndex;

out VS_OUT
{
    vec3 normal;
    vec3 color;
} vsOut;

void main()
{
    mat4 model = uModels[uTransformIndex];
    vec4 world = model * vec4(aPosition * 1.035, 1.0);
    vsOut.normal = mat3(transpose(inverse(model))) * aNormal;
    vsOut.color = aColor;
    gl_Position = uProjection * uView * world;
}
)GLSL";

const char* kWireFragmentShader = R"GLSL(
#version 460 core
layout (location = 0) out vec4 FragColor;

struct Material
{
    vec4 baseColor;
    vec4 params;
};

layout (std140, binding = 2) uniform MaterialData
{
    Material uMaterials[8];
};

uniform int uMaterialIndex;

in VS_OUT
{
    vec3 normal;
    vec3 color;
} fsIn;

void main()
{
    Material material = uMaterials[uMaterialIndex];
    vec3 n = normalize(fsIn.normal) * 0.5 + 0.5;
    vec3 color = mix(n, material.baseColor.rgb * fsIn.color, 0.35);
    FragColor = vec4(color, 1.0);
}
)GLSL";

void APIENTRY debugCallback(
    GLenum source,
    GLenum type,
    GLuint id,
    GLenum severity,
    GLsizei length,
    const GLchar* message,
    const void* userParam)
{
    (void)source;
    (void)type;
    (void)id;
    (void)length;
    (void)userParam;

    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
    {
        return;
    }

    std::cerr << "OpenGL debug: " << message << std::endl;
}

GLuint compileShader(GLenum type, const char* source)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE)
    {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<size_t>(logLength), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error("Shader compilation failed: " + log);
    }

    return shader;
}

GLuint createProgram(const char* vertexSource, const char* fragmentSource)
{
    GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success != GL_TRUE)
    {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<size_t>(logLength), '\0');
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        glDeleteProgram(program);
        throw std::runtime_error("Program link failed: " + log);
    }

    return program;
}

MeshGpu createCubeMesh()
{
    const std::array<Vertex, 24> vertices = {
        Vertex{{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.72f, 0.45f}},
        Vertex{{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.9f, 0.86f, 0.55f}},
        Vertex{{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.7f, 0.93f, 0.85f}},
        Vertex{{-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.55f, 0.72f, 1.0f}},

        Vertex{{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.9f, 0.55f, 0.8f}},
        Vertex{{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.52f, 0.7f, 0.95f}},
        Vertex{{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.65f, 0.9f, 0.7f}},
        Vertex{{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.76f, 0.45f}},

        Vertex{{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.78f, 0.68f, 1.0f}},
        Vertex{{-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {0.55f, 0.84f, 0.92f}},
        Vertex{{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {0.95f, 0.8f, 0.58f}},
        Vertex{{-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.7f, 0.92f, 0.6f}},

        Vertex{{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.8f, 0.62f, 1.0f}},
        Vertex{{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.68f, 0.5f}},
        Vertex{{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.56f, 0.86f, 0.86f}},
        Vertex{{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.68f, 0.75f, 1.0f}},

        Vertex{{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.92f, 0.8f, 0.5f}},
        Vertex{{ 0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.75f, 0.95f, 0.82f}},
        Vertex{{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.55f, 0.7f, 1.0f}},
        Vertex{{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.95f, 0.56f, 0.72f}},

        Vertex{{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.56f, 0.82f, 0.95f}},
        Vertex{{ 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.95f, 0.72f, 0.45f}},
        Vertex{{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {0.7f, 0.92f, 0.7f}},
        Vertex{{-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {0.82f, 0.64f, 1.0f}},
    };

    const std::array<std::uint32_t, 36> indices = {
        0, 1, 2, 2, 3, 0,
        4, 5, 6, 6, 7, 4,
        8, 9, 10, 10, 11, 8,
        12, 13, 14, 14, 15, 12,
        16, 17, 18, 18, 19, 16,
        20, 21, 22, 22, 23, 20,
    };

    MeshGpu mesh{};
    mesh.indexCount = static_cast<GLsizei>(indices.size());

    glCreateVertexArrays(1, &mesh.vao);
    glCreateBuffers(1, &mesh.vbo);
    glCreateBuffers(1, &mesh.ebo);

    glNamedBufferData(mesh.vbo, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STATIC_DRAW);
    glNamedBufferData(mesh.ebo, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data(), GL_STATIC_DRAW);

    glVertexArrayVertexBuffer(mesh.vao, 0, mesh.vbo, 0, sizeof(Vertex));
    glVertexArrayElementBuffer(mesh.vao, mesh.ebo);

    glEnableVertexArrayAttrib(mesh.vao, 0);
    glVertexArrayAttribFormat(mesh.vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribBinding(mesh.vao, 0, 0);

    glEnableVertexArrayAttrib(mesh.vao, 1);
    glVertexArrayAttribFormat(mesh.vao, 1, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, normal));
    glVertexArrayAttribBinding(mesh.vao, 1, 0);

    glEnableVertexArrayAttrib(mesh.vao, 2);
    glVertexArrayAttribFormat(mesh.vao, 2, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, color));
    glVertexArrayAttribBinding(mesh.vao, 2, 0);

    return mesh;
}

GLuint createUniformBuffer(GLsizeiptr size, const void* data, GLenum usage)
{
    GLuint buffer = 0;
    glCreateBuffers(1, &buffer);
    glNamedBufferData(buffer, size, data, usage);
    return buffer;
}

std::vector<glm::vec3> createCubePositions()
{
    std::vector<glm::vec3> positions;
    positions.reserve(kCubeCount);

    constexpr int side = 4;
    constexpr float spacing = 2.25f;
    const float offset = static_cast<float>(side - 1) * spacing * 0.5f;

    for (int z = 0; z < side; ++z)
    {
        for (int y = 0; y < side; ++y)
        {
            for (int x = 0; x < side; ++x)
            {
                positions.emplace_back(
                    static_cast<float>(x) * spacing - offset,
                    static_cast<float>(y) * spacing - offset,
                    static_cast<float>(z) * spacing - offset);
            }
        }
    }

    return positions;
}

std::array<MaterialGpu, kMaterialCount> createMaterials()
{
    return {
        MaterialGpu{{0.95f, 0.48f, 0.32f, 1.0f}, {48.0f, 0.45f, 0.0f, 0.0f}},
        MaterialGpu{{0.34f, 0.78f, 0.92f, 1.0f}, {72.0f, 0.55f, 0.0f, 0.0f}},
        MaterialGpu{{0.62f, 0.86f, 0.48f, 1.0f}, {56.0f, 0.38f, 0.0f, 0.0f}},
        MaterialGpu{{0.93f, 0.76f, 0.35f, 1.0f}, {84.0f, 0.60f, 0.0f, 0.0f}},
        MaterialGpu{{0.74f, 0.54f, 0.96f, 1.0f}, {42.0f, 0.50f, 0.0f, 0.0f}},
        MaterialGpu{{0.20f, 0.95f, 0.74f, 1.0f}, {64.0f, 0.48f, 0.0f, 0.0f}},
        MaterialGpu{{1.00f, 0.42f, 0.66f, 1.0f}, {40.0f, 0.35f, 0.0f, 0.0f}},
        MaterialGpu{{0.38f, 0.60f, 1.00f, 1.0f}, {90.0f, 0.62f, 0.0f, 0.0f}},
    };
}

std::vector<CachedDrawCommand> buildCachedCommands(GLuint litProgram, GLuint wireProgram, const MeshGpu& mesh)
{
    std::vector<CachedDrawCommand> commands;
    commands.reserve(kCubeCount);

    for (GLuint i = 0; i < static_cast<GLuint>(kCubeCount); ++i)
    {
        const bool lit = (i % 3) != 0;
        CachedDrawCommand command{};
        command.pipeline = lit ? PipelineId::Lit : PipelineId::Wire;
        command.program = lit ? litProgram : wireProgram;
        command.vao = mesh.vao;
        command.indexCount = mesh.indexCount;
        command.transformIndex = i;
        command.materialIndex = i % kMaterialCount;
        command.drawMode = GL_TRIANGLES;
        command.depthTest = true;
        command.cullFace = lit;
        command.wireframe = !lit;
        command.visible = true;
        commands.push_back(command);
    }

    std::stable_sort(commands.begin(), commands.end(), [](const CachedDrawCommand& lhs, const CachedDrawCommand& rhs) {
        if (lhs.pipeline != rhs.pipeline)
        {
            return lhs.pipeline < rhs.pipeline;
        }
        if (lhs.program != rhs.program)
        {
            return lhs.program < rhs.program;
        }
        if (lhs.vao != rhs.vao)
        {
            return lhs.vao < rhs.vao;
        }
        if (lhs.depthTest != rhs.depthTest)
        {
            return lhs.depthTest > rhs.depthTest;
        }
        if (lhs.cullFace != rhs.cullFace)
        {
            return lhs.cullFace > rhs.cullFace;
        }
        return lhs.wireframe < rhs.wireframe;
    });

    return commands;
}

glm::vec3 getForward(const Camera& camera)
{
    const float yawRadians = glm::radians(camera.yaw);
    const float pitchRadians = glm::radians(camera.pitch);
    return glm::normalize(glm::vec3(
        std::cos(yawRadians) * std::cos(pitchRadians),
        std::sin(pitchRadians),
        std::sin(yawRadians) * std::cos(pitchRadians)));
}

void handleInput(GLFWwindow* window, AppState& state, float deltaSeconds)
{
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
    {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    const bool spacePressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    if (spacePressed && !state.previousSpace)
    {
        state.animate = !state.animate;
    }
    state.previousSpace = spacePressed;

    const bool onePressed = glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS;
    if (onePressed && !state.previousOne)
    {
        state.showLit = !state.showLit;
    }
    state.previousOne = onePressed;

    const bool twoPressed = glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS;
    if (twoPressed && !state.previousTwo)
    {
        state.showWire = !state.showWire;
    }
    state.previousTwo = twoPressed;

    const glm::vec3 forward = getForward(state.camera);
    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
    const float speed = 7.5f * deltaSeconds;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
    {
        state.camera.position += forward * speed;
    }
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
    {
        state.camera.position -= forward * speed;
    }
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
    {
        state.camera.position -= right * speed;
    }
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
    {
        state.camera.position += right * speed;
    }
}

std::array<glm::mat4, 128> buildTransforms(const std::vector<glm::vec3>& positions, float timeSeconds, bool animate)
{
    std::array<glm::mat4, 128> transforms{};
    (void)animate;
    const float animationTime = timeSeconds;

    for (size_t i = 0; i < positions.size(); ++i)
    {
        const float fi = static_cast<float>(i);
        glm::vec3 position = positions[i];
        position.y += std::sin(animationTime * 1.25f + fi * 0.37f) * 0.18f;

        glm::mat4 model(1.0f);
        model = glm::translate(model, position);
        model = glm::rotate(model, animationTime * (0.35f + static_cast<float>(i % 5) * 0.06f), glm::normalize(glm::vec3(0.35f, 1.0f, 0.2f)));
        model = glm::scale(model, glm::vec3(0.86f));
        transforms[i] = model;
    }

    return transforms;
}

void setCapability(GLenum capability, bool enabled, bool& cachedValue)
{
    if (cachedValue == enabled)
    {
        return;
    }

    if (enabled)
    {
        glEnable(capability);
    }
    else
    {
        glDisable(capability);
    }
    cachedValue = enabled;
}

void replayCachedCommands(
    const std::vector<CachedDrawCommand>& commands,
    const AppState& state,
    GLint litTransformLocation,
    GLint litMaterialLocation,
    GLint wireTransformLocation,
    GLint wireMaterialLocation)
{
    GLuint currentProgram = 0;
    GLuint currentVao = 0;
    bool depthEnabled = true;
    bool cullEnabled = true;
    bool wireEnabled = false;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    for (const CachedDrawCommand& command : commands)
    {
        const bool visibleByPipeline =
            (command.pipeline == PipelineId::Lit && state.showLit) ||
            (command.pipeline == PipelineId::Wire && state.showWire);

        if (!command.visible || !visibleByPipeline)
        {
            continue;
        }

        if (currentProgram != command.program)
        {
            glUseProgram(command.program);
            currentProgram = command.program;
        }

        if (currentVao != command.vao)
        {
            glBindVertexArray(command.vao);
            currentVao = command.vao;
        }

        setCapability(GL_DEPTH_TEST, command.depthTest, depthEnabled);
        setCapability(GL_CULL_FACE, command.cullFace, cullEnabled);

        if (wireEnabled != command.wireframe)
        {
            glPolygonMode(GL_FRONT_AND_BACK, command.wireframe ? GL_LINE : GL_FILL);
            wireEnabled = command.wireframe;
        }

        const GLint transformLocation = command.pipeline == PipelineId::Lit ? litTransformLocation : wireTransformLocation;
        const GLint materialLocation = command.pipeline == PipelineId::Lit ? litMaterialLocation : wireMaterialLocation;
        glUniform1i(transformLocation, static_cast<GLint>(command.transformIndex));
        glUniform1i(materialLocation, static_cast<GLint>(command.materialIndex));
        glDrawElements(command.drawMode, command.indexCount, GL_UNSIGNED_INT, nullptr);
    }

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glBindVertexArray(0);
    glUseProgram(0);
}

void framebufferSizeCallback(GLFWwindow*, int width, int height)
{
    glViewport(0, 0, width, height);
}
} // namespace

int main()
{
    try
    {
        if (glfwInit() != GLFW_TRUE)
        {
            std::cerr << "Failed to initialize GLFW." << std::endl;
            return 1;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);

        GLFWwindow* window = glfwCreateWindow(kWindowWidth, kWindowHeight, "OpenGL CachedCubePipelines Test", nullptr, nullptr);
        if (!window)
        {
            std::cerr << "Failed to create an OpenGL 4.6 core profile context." << std::endl;
            glfwTerminate();
            return 1;
        }

        glfwMakeContextCurrent(window);
        glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
        glfwSwapInterval(1);

        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
        {
            std::cerr << "Failed to load OpenGL functions through GLAD." << std::endl;
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;
        std::cout << "OpenGL vendor: " << glGetString(GL_VENDOR) << std::endl;
        std::cout << "OpenGL renderer: " << glGetString(GL_RENDERER) << std::endl;

        GLint major = 0;
        GLint minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        if (major < 4 || (major == 4 && minor < 6))
        {
            std::cerr << "OpenGL 4.6 is required, but the current context is "
                      << major << "." << minor << "." << std::endl;
            glfwDestroyWindow(window);
            glfwTerminate();
            return 1;
        }

        if (GLAD_GL_VERSION_4_3)
        {
            glEnable(GL_DEBUG_OUTPUT);
            glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            glDebugMessageCallback(debugCallback, nullptr);
        }

        glViewport(0, 0, kWindowWidth, kWindowHeight);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glClearColor(0.045f, 0.055f, 0.065f, 1.0f);

        const GLuint litProgram = createProgram(kLitVertexShader, kLitFragmentShader);
        const GLuint wireProgram = createProgram(kWireVertexShader, kWireFragmentShader);
        const MeshGpu cubeMesh = createCubeMesh();
        const std::vector<glm::vec3> cubePositions = createCubePositions();
        const std::array<MaterialGpu, kMaterialCount> materials = createMaterials();
        const std::vector<CachedDrawCommand> cachedCommands = buildCachedCommands(litProgram, wireProgram, cubeMesh);

        const GLint litTransformLocation = glGetUniformLocation(litProgram, "uTransformIndex");
        const GLint litMaterialLocation = glGetUniformLocation(litProgram, "uMaterialIndex");
        const GLint wireTransformLocation = glGetUniformLocation(wireProgram, "uTransformIndex");
        const GLint wireMaterialLocation = glGetUniformLocation(wireProgram, "uMaterialIndex");

        FrameGpu frameData{};
        const GLuint frameUbo = createUniformBuffer(sizeof(FrameGpu), &frameData, GL_DYNAMIC_DRAW);
        const GLuint transformUbo = createUniformBuffer(sizeof(glm::mat4) * 128, nullptr, GL_DYNAMIC_DRAW);
        const GLuint materialUbo = createUniformBuffer(sizeof(MaterialGpu) * materials.size(), materials.data(), GL_STATIC_DRAW);

        glBindBufferBase(GL_UNIFORM_BUFFER, kFrameBinding, frameUbo);
        glBindBufferBase(GL_UNIFORM_BUFFER, kTransformBinding, transformUbo);
        glBindBufferBase(GL_UNIFORM_BUFFER, kMaterialBinding, materialUbo);

        AppState app{};
        float accumulatedTime = 0.0f;
        float previousTime = static_cast<float>(glfwGetTime());

        while (!glfwWindowShouldClose(window))
        {
            const float currentTime = static_cast<float>(glfwGetTime());
            const float deltaSeconds = std::min(currentTime - previousTime, 0.05f);
            previousTime = currentTime;

            handleInput(window, app, deltaSeconds);
            if (app.animate)
            {
                accumulatedTime += deltaSeconds;
            }

            int framebufferWidth = 0;
            int framebufferHeight = 0;
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
            const float aspectRatio = framebufferHeight > 0
                ? static_cast<float>(framebufferWidth) / static_cast<float>(framebufferHeight)
                : 16.0f / 9.0f;

            const glm::vec3 forward = getForward(app.camera);
            const glm::vec3 lightPosition = glm::vec3(
                std::cos(accumulatedTime * 0.7f) * 8.0f,
                8.0f + std::sin(accumulatedTime * 0.4f) * 2.0f,
                std::sin(accumulatedTime * 0.7f) * 8.0f);

            frameData.view = glm::lookAt(app.camera.position, app.camera.position + forward, glm::vec3(0.0f, 1.0f, 0.0f));
            frameData.projection = glm::perspective(glm::radians(60.0f), aspectRatio, 0.1f, 200.0f);
            frameData.cameraPosition = glm::vec4(app.camera.position, 1.0f);
            frameData.lightPosition = glm::vec4(lightPosition, 1.0f);
            frameData.timeInfo = glm::vec4(accumulatedTime, deltaSeconds, app.animate ? 1.0f : 0.0f, 0.0f);

            const std::array<glm::mat4, 128> transforms = buildTransforms(cubePositions, accumulatedTime, app.animate);
            glNamedBufferSubData(frameUbo, 0, sizeof(FrameGpu), &frameData);
            glNamedBufferSubData(transformUbo, 0, static_cast<GLsizeiptr>(sizeof(glm::mat4) * transforms.size()), transforms.data());

            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            replayCachedCommands(cachedCommands, app, litTransformLocation, litMaterialLocation, wireTransformLocation, wireMaterialLocation);

            glfwSwapBuffers(window);
            glfwPollEvents();
        }

        glDeleteBuffers(1, &materialUbo);
        glDeleteBuffers(1, &transformUbo);
        glDeleteBuffers(1, &frameUbo);
        glDeleteBuffers(1, &cubeMesh.ebo);
        glDeleteBuffers(1, &cubeMesh.vbo);
        glDeleteVertexArrays(1, &cubeMesh.vao);
        glDeleteProgram(wireProgram);
        glDeleteProgram(litProgram);

        glfwDestroyWindow(window);
        glfwTerminate();
    }
    catch (const std::exception& ex)
    {
        std::cerr << ex.what() << std::endl;
        glfwTerminate();
        return 1;
    }

    return 0;
}
