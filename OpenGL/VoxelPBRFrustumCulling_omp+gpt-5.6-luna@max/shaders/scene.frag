#version 460 core

layout(location = 0) out vec4 FragColor;

layout(std140, binding = 0) uniform FrameBlock {
    mat4 uViewProjection;
    mat4 uLightSpace;
    vec4 uCameraPosition;
    vec4 uSunDirection;
    vec4 uSunRadiance;
    vec4 uPointPositions[4];
    vec4 uPointColors[4];
};

in vec3 vWorldPosition;
in vec3 vWorldNormal;
in vec3 vWorldTangent;
in vec3 vWorldBitangent;
in vec2 vUV;
in vec4 vLightSpacePosition;
flat in int vMaterialIndex;

layout(binding = 0) uniform sampler2DArray uBaseColorTex;
layout(binding = 1) uniform sampler2DArray uNormalTex;
layout(binding = 2) uniform sampler2DArray uOrmTex;
layout(binding = 3) uniform samplerCube uIrradianceMap;
layout(binding = 4) uniform samplerCube uPrefilterMap;
layout(binding = 5) uniform sampler2D uBrdfLut;
layout(binding = 6) uniform sampler2DShadow uShadowMap;
uniform int uDebugMode;

const float PI = 3.14159265359;

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float NdotH = max(dot(N, H), 0.0);
    float denominator = NdotH * NdotH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 0.000001);
}

float geometrySchlickGGX(float NdotV, float roughness) {
    float alpha = roughness * roughness;
    float k = alpha * 0.5;
    return NdotV / max(NdotV * (1.0 - k) + k, 0.000001);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
           geometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) *
           pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

vec3 getNormal(vec3 geometricNormal) {
    vec3 tangent = normalize(vWorldTangent - geometricNormal * dot(geometricNormal, vWorldTangent));
    vec3 bitangent = normalize(cross(geometricNormal, tangent));
    if (dot(bitangent, normalize(vWorldBitangent)) < 0.0) {
        bitangent = -bitangent;
    }
    vec3 tangentNormal = texture(uNormalTex, vec3(vUV, float(vMaterialIndex))).xyz * 2.0 - 1.0;
    tangentNormal.z = sqrt(max(1.0 - dot(tangentNormal.xy, tangentNormal.xy), 0.0));
    return normalize(mat3(tangent, bitangent, geometricNormal) * tangentNormal);
}

float shadowVisibility(vec3 N, vec3 L) {
    vec3 projected = vLightSpacePosition.xyz / max(vLightSpacePosition.w, 0.000001);
    projected = projected * 0.5 + 0.5;
    if (projected.z > 1.0 || projected.x < 0.0 || projected.x > 1.0 || projected.y < 0.0 || projected.y > 1.0) {
        return 1.0;
    }
    float NdotL = max(dot(N, L), 0.0);
    float bias = max(0.0008 * (1.0 - NdotL), 0.00008);
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));
    float visibility = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            visibility += texture(uShadowMap, vec3(projected.xy + vec2(x, y) * texelSize, projected.z - bias));
        }
    }
    return visibility / 9.0;
}

vec3 evaluateDirect(vec3 N, vec3 V, vec3 albedo, float metallic, float roughness, vec3 F0) {
    vec3 result = vec3(0.0);
    vec3 sunL = normalize(-uSunDirection.xyz);
    float sunNdotL = max(dot(N, sunL), 0.0);
    if (sunNdotL > 0.0) {
        vec3 H = normalize(V + sunL);
        float NDF = distributionGGX(N, H, roughness);
        float geometry = geometrySmith(N, V, sunL, roughness);
        vec3 fresnel = fresnelSchlick(max(dot(H, V), 0.0), F0);
        vec3 numerator = NDF * geometry * fresnel;
        float denominator = max(4.0 * max(dot(N, V), 0.0) * sunNdotL, 0.0001);
        vec3 specular = numerator / denominator;
        vec3 kS = fresnel;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
        float shadow = shadowVisibility(N, sunL);
        result += (kD * albedo / PI + specular) * uSunRadiance.xyz * sunNdotL * shadow;
    }

    for (int index = 0; index < 4; ++index) {
        vec3 toLight = uPointPositions[index].xyz - vWorldPosition;
        float distanceSquared = max(dot(toLight, toLight), 0.0001);
        vec3 pointL = toLight * inversesqrt(distanceSquared);
        float pointNdotL = max(dot(N, pointL), 0.0);
        if (pointNdotL <= 0.0) {
            continue;
        }
        vec3 H = normalize(V + pointL);
        float NDF = distributionGGX(N, H, roughness);
        float geometry = geometrySmith(N, V, pointL, roughness);
        vec3 fresnel = fresnelSchlick(max(dot(H, V), 0.0), F0);
        vec3 specular = (NDF * geometry * fresnel) /
                        max(4.0 * max(dot(N, V), 0.0) * pointNdotL, 0.0001);
        vec3 kS = fresnel;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
        vec3 radiance = uPointColors[index].xyz / distanceSquared;
        result += (kD * albedo / PI + specular) * radiance * pointNdotL;
    }
    return result;
}

vec3 acesToneMap(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / max(color * (c * color + d) + e, vec3(0.0001)), 0.0, 1.0);
}

void main() {
    vec3 sampledAlbedo = texture(uBaseColorTex, vec3(vUV, float(vMaterialIndex))).rgb;
    vec3 orm = texture(uOrmTex, vec3(vUV, float(vMaterialIndex))).rgb;
    float ambientOcclusion = orm.r;
    float roughness = clamp(orm.g, 0.04, 1.0);
    float metallic = clamp(orm.b, 0.0, 1.0);
    vec3 geometricNormal = normalize(vWorldNormal);
    vec3 N = getNormal(geometricNormal);
    vec3 V = normalize(uCameraPosition.xyz - vWorldPosition);
    float NdotV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), sampledAlbedo, metallic);

    vec3 displayLinear;
    if (uDebugMode == 2) {
        displayLinear = sampledAlbedo;
    } else if (uDebugMode == 3) {
        displayLinear = N * 0.5 + 0.5;
    } else if (uDebugMode == 4) {
        displayLinear = vec3(roughness, metallic, 0.0);
    } else if (uDebugMode == 5) {
        displayLinear = vec3(shadowVisibility(N, normalize(-uSunDirection.xyz)));
    } else {
        vec3 direct = evaluateDirect(N, V, sampledAlbedo, metallic, roughness, F0);
        vec3 viewFresnel = fresnelSchlickRoughness(NdotV, F0, roughness);
        vec3 kS = viewFresnel;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
        vec3 diffuse = texture(uIrradianceMap, N).rgb * sampledAlbedo;
        vec3 reflection = reflect(-V, N);
        vec3 prefilteredColor = textureLod(uPrefilterMap, reflection, roughness * 4.0).rgb;
        vec2 brdf = texture(uBrdfLut, vec2(NdotV, roughness)).rg;
        vec3 specular = prefilteredColor * (viewFresnel * brdf.x + brdf.y);
        vec3 ambient = (kD * diffuse + specular) * ambientOcclusion;
        displayLinear = ambient + direct;
    }

    vec3 toneMapped = acesToneMap(max(displayLinear, vec3(0.0)));
    FragColor = vec4(pow(toneMapped, vec3(1.0 / 2.2)), 1.0);
}
