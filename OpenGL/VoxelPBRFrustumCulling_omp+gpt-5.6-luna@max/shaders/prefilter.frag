#version 460 core

layout(location = 0) out vec4 FragColor;
layout(binding = 0) uniform samplerCube uEnvironmentMap;
uniform float uRoughness;
in vec3 vDirection;

const float PI = 3.14159265359;

float radicalInverseVdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint index, uint count) {
    return vec2(float(index) / float(count), radicalInverseVdC(index));
}

vec3 importanceSampleGGX(vec2 xi, vec3 normal, float roughness) {
    float alpha = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    vec3 halfVector = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * halfVector.x + bitangent * halfVector.y + normal * halfVector.z);
}

float distributionGGX(vec3 normal, vec3 halfVector, float roughness) {
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float NdotH = max(dot(normal, halfVector), 0.0);
    float denominator = NdotH * NdotH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 0.000001);
}

void main() {
    vec3 normal = normalize(vDirection);
    vec3 view = normal;
    vec3 prefilteredColor = vec3(0.0);
    float totalWeight = 0.0;
    const uint sampleCount = 512u;
    for (uint index = 0u; index < sampleCount; ++index) {
        vec2 xi = hammersley(index, sampleCount);
        vec3 halfVector = importanceSampleGGX(xi, normal, max(uRoughness, 0.001));
        vec3 light = normalize(2.0 * dot(view, halfVector) * halfVector - view);
        float NdotL = max(dot(normal, light), 0.0);
        if (NdotL <= 0.0) {
            continue;
        }
        float NdotH = max(dot(normal, halfVector), 0.0);
        float HdotV = max(dot(halfVector, view), 0.0);
        float distribution = distributionGGX(normal, halfVector, max(uRoughness, 0.001));
        float pdf = max(distribution * NdotH / max(4.0 * HdotV, 0.0001), 0.0001);
        float sampleSolidAngle = 1.0 / (float(sampleCount) * pdf + 0.0001);
        float texelSolidAngle = 4.0 * PI / (6.0 * 256.0 * 256.0);
        float mipLevel = uRoughness < 0.001 ? 0.0 : max(0.0, 0.5 * log2(sampleSolidAngle / texelSolidAngle));
        mipLevel = max(mipLevel, uRoughness * 4.0);
        prefilteredColor += textureLod(uEnvironmentMap, light, mipLevel).rgb * NdotL;
        totalWeight += NdotL;
    }
    FragColor = vec4(prefilteredColor / max(totalWeight, 0.0001), 1.0);
}
