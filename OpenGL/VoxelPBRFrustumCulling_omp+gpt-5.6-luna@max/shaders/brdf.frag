#version 460 core

layout(location = 0) out vec4 FragColor;
in vec2 vUV;

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

float geometrySchlickGGX(float NdotV, float roughness) {
    float k = roughness * roughness * 0.5;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.000001);
}

float geometrySmith(float NdotV, float NdotL, float roughness) {
    return geometrySchlickGGX(NdotV, roughness) * geometrySchlickGGX(NdotL, roughness);
}

vec2 integrateBRDF(float NdotV, float roughness) {
    vec3 view = vec3(sqrt(max(1.0 - NdotV * NdotV, 0.0)), 0.0, NdotV);
    float scale = 0.0;
    float bias = 0.0;
    const uint sampleCount = 512u;
    for (uint index = 0u; index < sampleCount; ++index) {
        vec2 xi = hammersley(index, sampleCount);
        vec3 halfVector = importanceSampleGGX(xi, vec3(0.0, 0.0, 1.0), roughness);
        vec3 light = normalize(2.0 * dot(view, halfVector) * halfVector - view);
        float NdotL = max(light.z, 0.0);
        float NdotH = max(halfVector.z, 0.0);
        float VdotH = max(dot(view, halfVector), 0.0);
        if (NdotL > 0.0) {
            float visibility = geometrySmith(NdotV, NdotL, roughness) * VdotH /
                               max(NdotH * NdotV, 0.000001);
            float fresnel = pow(1.0 - VdotH, 5.0);
            scale += (1.0 - fresnel) * visibility;
            bias += fresnel * visibility;
        }
    }
    return vec2(scale, bias) / float(sampleCount);
}

void main() {
    vec2 integrated = integrateBRDF(clamp(vUV.x, 0.0, 1.0), clamp(vUV.y, 0.0, 1.0));
    FragColor = vec4(integrated, 0.0, 1.0);
}
