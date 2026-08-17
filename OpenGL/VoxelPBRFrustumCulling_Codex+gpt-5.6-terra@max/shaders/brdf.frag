#version 460 core

layout (location = 0) out vec2 FragColor;
in vec2 vUv;

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

vec3 ImportanceSampleGGX(vec2 xi, float roughness)
{
    float alpha = max(roughness * roughness, 0.001);
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    return vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float GeometrySchlickGGX(float normalDotDirection, float roughness)
{
    float alpha = roughness * roughness;
    float k = alpha * 0.5;
    return normalDotDirection / max(normalDotDirection * (1.0 - k) + k, 0.000001);
}

float GeometrySmith(float normalDotView, float normalDotLight, float roughness)
{
    return GeometrySchlickGGX(normalDotView, roughness) * GeometrySchlickGGX(normalDotLight, roughness);
}

vec2 IntegrateBrdf(float normalDotView, float roughness)
{
    vec3 viewDirection = vec3(sqrt(max(1.0 - normalDotView * normalDotView, 0.0)), 0.0, normalDotView);
    float scale = 0.0;
    float bias = 0.0;
    for (uint sampleIndex = 0u; sampleIndex < SampleCount; ++sampleIndex)
    {
        vec2 xi = Hammersley(sampleIndex, SampleCount);
        vec3 halfVector = ImportanceSampleGGX(xi, roughness);
        vec3 lightDirection = normalize(2.0 * dot(viewDirection, halfVector) * halfVector - viewDirection);
        float normalDotLight = max(lightDirection.z, 0.0);
        float normalDotHalf = max(halfVector.z, 0.0);
        float viewDotHalf = max(dot(viewDirection, halfVector), 0.0);
        if (normalDotLight > 0.0)
        {
            float geometry = GeometrySmith(normalDotView, normalDotLight, roughness);
            float visibility = geometry * viewDotHalf / max(normalDotHalf * normalDotView, 0.000001);
            float fresnel = pow(1.0 - viewDotHalf, 5.0);
            scale += (1.0 - fresnel) * visibility;
            bias += fresnel * visibility;
        }
    }
    return vec2(scale, bias) / float(SampleCount);
}

void main()
{
    FragColor = IntegrateBrdf(vUv.x, vUv.y);
}
