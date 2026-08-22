#version 460
#extension GL_EXT_nonuniform_qualifier : enable

// Cook-Torrance PBR 片元着色器（直接光照）
// - 基础色纹理：sRGB 语义，采样得到线性值
// - 粗糙度纹理：UNORM，单通道
// - 法线贴图：UNORM，RGB，由高度图推导
// - 材质参数（metallic/roughness/ambientOcclusion/normalStrength）经 push constant 传入
// - 输出写 sRGB 交换链图像（由硬件完成线性->sRGB），这里输出线性值

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragTangent;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 4) flat in uint fragMatIdx;

layout(location = 0) out vec4 outColor;

// 相机 UBO：与顶点着色器共用同一 set0 binding0
layout(set = 0, binding = 0) uniform CameraUbo {
    mat4 viewProj;
    vec3 camPos;
} camera;

// Push Descriptors：每个材质三张纹理（基础色/粗糙度/法线），按材质索引绑定
layout(set = 0, binding = 1) uniform sampler2D baseColorTex[];
layout(set = 0, binding = 2) uniform sampler2D roughnessTex[];
layout(set = 0, binding = 3) uniform sampler2D normalTex[];

// 材质参数通过 push constant 传入（来自 JSON，不在 shader 硬编码数值）
struct MaterialParams {
    vec4 baseColorLinearSRGBA; // xyz: 线性基础色, w: ambientOcclusion
    vec4 metallicRoughnessNormStrength; // x: metallic, y: roughness, z: normalStrength, w: unused
};

// 与顶点着色器共享同一 push constant 块（offset 必须一致）
layout(push_constant) uniform PushConstants {
    mat4 model;          // offset 0
    uint matIdx;         // offset 64
    // padding 12 字节使 matParams 16 字节对齐
    MaterialParams matParams; // offset 80
} pc;

// 光照参数：固定方向光 + 固定环境光（场景规格，非材质数据）
const vec3 LIGHT_DIR = normalize(vec3(-0.577, -0.577, -0.577));
const vec3 LIGHT_COLOR = vec3(1.0) * 4.0;
const float AMBIENT_STRENGTH = 0.03;

const float PI = 3.14159265359;

// sRGB -> 线性（用于从 sRGB 采样器拿到线性值后无需再转；这里采样器是 sRGB，自动转）
// 由于使用 VK_FORMAT_R8G8B8A8_SRGB 采样，sampler 返回值已是线性，无需手动转换。

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    return a2 / max(denom, 1e-7);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx2 * ggx1;
}

vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 法线贴图：从切线空间法线转换到世界空间
// 用顶点切线构建 TBN（切线空间约定：T=+U方向, B=cross(N,T)*手性, N=几何法线）
// 法线贴图由高度图 Sobel 推导，切线空间 xyz 与 image UV 的 xy 对应。
mat3 ComputeTBN(vec3 N, vec3 T) {
    T = normalize(T);
    // Gram-Schmidt 正交化 T 与 N
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T);
    return mat3(T, B, N);
}

void main() {
    uint idx = pc.matIdx;

    // 基础色：sRGB 纹理采样器自动转线性
    vec3 albedo = texture(baseColorTex[nonuniformEXT(idx)], fragTexCoord).rgb;
    // 粗糙度纹理（UNORM 单通道）
    float roughness = texture(roughnessTex[nonuniformEXT(idx)], fragTexCoord).r;
    // 法线贴图（UNORM RGB -> [-1,1]）
    vec3 nSample = texture(normalTex[nonuniformEXT(idx)], fragTexCoord).xyz * 2.0 - 1.0;

    float metallic = pc.matParams.metallicRoughnessNormStrength.x;
    float ao = pc.matParams.baseColorLinearSRGBA.w;
    float normalStrength = pc.matParams.metallicRoughnessNormStrength.z;

    // 世界空间法线 + 法线贴图扰动
    vec3 N = normalize(fragNormal);
    mat3 tbn = ComputeTBN(N, fragTangent);
    nSample.xy *= normalStrength;
    vec3 perturbedN = normalize(tbn * normalize(nSample));

    vec3 V = normalize(camera.camPos - fragWorldPos);
    vec3 L = normalize(-LIGHT_DIR);
    vec3 H = normalize(V + L);

    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    float NDF = DistributionGGX(perturbedN, H, roughness);
    float G = GeometrySmith(perturbedN, V, L, roughness);
    float NdotV = max(dot(perturbedN, V), 0.0);
    float NdotL = max(dot(perturbedN, L), 0.0);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 specular = (NDF * G * F) / max(4.0 * NdotV * NdotL, 1e-7);
    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    vec3 radiance = LIGHT_COLOR * NdotL;
    vec3 diffuse = kD * albedo / PI;
    vec3 Lo = (diffuse + specular) * radiance;

    vec3 ambient = vec3(AMBIENT_STRENGTH) * albedo * ao;
    vec3 color = ambient + Lo;

    // 输出线性值，由 sRGB 交换链格式完成最终转换
    outColor = vec4(color, 1.0);
}
