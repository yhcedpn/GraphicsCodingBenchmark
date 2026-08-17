#version 460 core

layout (location = 0) out vec4 FragColor;
in vec3 vLocalPosition;

uniform samplerCube uEnvironment;

const float PI = 3.14159265359;
const uint SampleCount = 512u;

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0Fu) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint index, uint count)
{
    return vec2(float(index) / float(count), RadicalInverseVdC(index));
}

void main()
{
    vec3 normal = normalize(vLocalPosition);
    vec3 up = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, normal));
    up = normalize(cross(normal, right));
    vec3 irradiance = vec3(0.0);

    for (uint sampleIndex = 0u; sampleIndex < SampleCount; ++sampleIndex)
    {
        vec2 xi = Hammersley(sampleIndex, SampleCount);
        float phi = 2.0 * PI * xi.x;
        float cosTheta = sqrt(1.0 - xi.y);
        float sinTheta = sqrt(xi.y);
        vec3 tangentSample = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
        vec3 sampleVector = tangentSample.x * right + tangentSample.y * up + tangentSample.z * normal;
        irradiance += texture(uEnvironment, sampleVector).rgb;
    }
    irradiance = PI * irradiance / float(SampleCount);
    FragColor = vec4(irradiance, 1.0);
}
