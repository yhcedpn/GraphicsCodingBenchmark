#version 460 core

layout (location = 0) out vec4 FragColor;
in vec3 vLocalPosition;

uniform samplerCube uEnvironment;
uniform float uRoughness;
uniform float uEnvironmentResolution;

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

vec3 ImportanceSampleGGX(vec2 xi, vec3 normal, float roughness)
{
    float alpha = max(roughness * roughness, 0.001);
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    vec3 halfVectorTangent = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * halfVectorTangent.x + bitangent * halfVectorTangent.y + normal * halfVectorTangent.z);
}

float DistributionGGX(vec3 normal, vec3 halfVector, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float normalDotHalf = max(dot(normal, halfVector), 0.0);
    float normalDotHalfSquared = normalDotHalf * normalDotHalf;
    float denominator = normalDotHalfSquared * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 0.000001);
}

void main()
{
    vec3 normal = normalize(vLocalPosition);
    vec3 viewDirection = normal;
    vec3 prefilteredColor = vec3(0.0);
    float totalWeight = 0.0;
    float texelSolidAngle = 4.0 * PI / (6.0 * uEnvironmentResolution * uEnvironmentResolution);

    for (uint sampleIndex = 0u; sampleIndex < SampleCount; ++sampleIndex)
    {
        vec2 xi = Hammersley(sampleIndex, SampleCount);
        vec3 halfVector = ImportanceSampleGGX(xi, normal, uRoughness);
        vec3 lightDirection = normalize(2.0 * dot(viewDirection, halfVector) * halfVector - viewDirection);
        float normalDotLight = max(dot(normal, lightDirection), 0.0);
        if (normalDotLight > 0.0)
        {
            float distribution = DistributionGGX(normal, halfVector, uRoughness);
            float normalDotHalf = max(dot(normal, halfVector), 0.0);
            float halfDotView = max(dot(halfVector, viewDirection), 0.0);
            float pdf = max(distribution * normalDotHalf / max(4.0 * halfDotView, 0.000001), 0.000001);
            float sampleSolidAngle = 1.0 / (float(SampleCount) * pdf + 0.000001);
            float mipLevel = uRoughness <= 0.0 ? 0.0 : max(0.5 * log2(sampleSolidAngle / texelSolidAngle), 0.0);
            prefilteredColor += textureLod(uEnvironment, lightDirection, mipLevel).rgb * normalDotLight;
            totalWeight += normalDotLight;
        }
    }
    FragColor = vec4(prefilteredColor / max(totalWeight, 0.000001), 1.0);
}
