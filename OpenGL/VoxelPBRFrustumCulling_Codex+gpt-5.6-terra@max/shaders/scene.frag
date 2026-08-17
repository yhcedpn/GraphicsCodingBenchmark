#version 460 core

layout (location = 0) out vec4 FragColor;

in VS_OUT
{
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 uv;
    vec4 lightSpacePosition;
    flat uint materialIndex;
} fsIn;

layout (std140, binding = 0) uniform FrameData
{
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpace;
    vec4 uCameraPosition;
    vec4 uSunDirection;
    vec4 uSunRadiance;
    vec4 uPointPositions[4];
    vec4 uPointRadiances[4];
    ivec4 uSettings;
};

uniform sampler2DArray uBaseColor;
uniform sampler2DArray uNormalMap;
uniform sampler2DArray uOrm;
uniform sampler2D uShadowMap;
uniform samplerCube uIrradiance;
uniform samplerCube uPrefilter;
uniform sampler2D uBrdfLut;

const float PI = 3.14159265359;

float DistributionGGX(vec3 normal, vec3 halfVector, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float normalDotHalf = max(dot(normal, halfVector), 0.0);
    float normalDotHalfSquared = normalDotHalf * normalDotHalf;
    float denominator = normalDotHalfSquared * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(PI * denominator * denominator, 0.000001);
}

float GeometrySchlickGGX(float normalDotView, float roughness)
{
    float roughnessPlusOne = roughness + 1.0;
    float k = (roughnessPlusOne * roughnessPlusOne) / 8.0;
    return normalDotView / max(normalDotView * (1.0 - k) + k, 0.000001);
}

float GeometrySmith(vec3 normal, vec3 viewDirection, vec3 lightDirection, float roughness)
{
    float normalDotView = max(dot(normal, viewDirection), 0.0);
    float normalDotLight = max(dot(normal, lightDirection), 0.0);
    return GeometrySchlickGGX(normalDotView, roughness) * GeometrySchlickGGX(normalDotLight, roughness);
}

vec3 FresnelSchlick(float cosine, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

vec3 FresnelSchlickRoughness(float cosine, vec3 f0, float roughness)
{
    return f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

mat3 BuildTangentFrame(vec3 normal, vec3 worldPosition, vec2 uv)
{
    vec3 positionDx = dFdx(worldPosition);
    vec3 positionDy = dFdy(worldPosition);
    vec2 uvDx = dFdx(uv);
    vec2 uvDy = dFdy(uv);
    vec3 perpendicularDy = cross(positionDy, normal);
    vec3 perpendicularDx = cross(normal, positionDx);
    vec3 tangent = perpendicularDy * uvDx.x + perpendicularDx * uvDy.x;
    vec3 bitangent = perpendicularDy * uvDx.y + perpendicularDx * uvDy.y;
    float scale = inversesqrt(max(dot(tangent, tangent), dot(bitangent, bitangent)));
    return mat3(tangent * scale, bitangent * scale, normal);
}

float DirectionalShadow(vec4 lightSpacePosition, vec3 normal, vec3 lightDirection)
{
    vec3 projected = lightSpacePosition.xyz / max(lightSpacePosition.w, 0.00001);
    projected = projected * 0.5 + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0 || projected.x <= 0.0 || projected.x >= 1.0 || projected.y <= 0.0 || projected.y >= 1.0)
    {
        return 1.0;
    }

    float bias = max(0.0025 * (1.0 - max(dot(normal, lightDirection), 0.0)), 0.00035);
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));
    float shadow = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float storedDepth = texture(uShadowMap, projected.xy + vec2(x, y) * texelSize).r;
            shadow += projected.z - bias > storedDepth ? 1.0 : 0.0;
        }
    }
    return 1.0 - shadow / 9.0;
}

vec3 EvaluateDirectLight(vec3 normal, vec3 viewDirection, vec3 lightDirection, vec3 radiance,
    vec3 albedo, vec3 f0, float metallic, float roughness)
{
    vec3 halfVector = normalize(viewDirection + lightDirection);
    float distribution = DistributionGGX(normal, halfVector, roughness);
    float geometry = GeometrySmith(normal, viewDirection, lightDirection, roughness);
    vec3 fresnel = FresnelSchlick(max(dot(halfVector, viewDirection), 0.0), f0);
    vec3 numerator = distribution * geometry * fresnel;
    float denominator = max(4.0 * max(dot(normal, viewDirection), 0.0) * max(dot(normal, lightDirection), 0.0), 0.0001);
    vec3 specular = numerator / denominator;
    vec3 kS = fresnel;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    float normalDotLight = max(dot(normal, lightDirection), 0.0);
    return (kD * albedo / PI + specular) * radiance * normalDotLight;
}

void main()
{
    float materialLayer = float(fsIn.materialIndex);
    vec3 albedo = texture(uBaseColor, vec3(fsIn.uv, materialLayer)).rgb;
    vec3 tangentNormal = texture(uNormalMap, vec3(fsIn.uv, materialLayer)).rgb * 2.0 - 1.0;
    vec3 orm = texture(uOrm, vec3(fsIn.uv, materialLayer)).rgb;
    float ambientOcclusion = orm.r;
    float roughness = clamp(orm.g, 0.045, 1.0);
    float metallic = clamp(orm.b, 0.0, 1.0);
    vec3 normal = normalize(BuildTangentFrame(normalize(fsIn.worldNormal), fsIn.worldPosition, fsIn.uv) * tangentNormal);
    vec3 viewDirection = normalize(uCameraPosition.xyz - fsIn.worldPosition);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    vec3 sunLightDirection = normalize(-uSunDirection.xyz);
    float shadowFactor = DirectionalShadow(fsIn.lightSpacePosition, normal, sunLightDirection);
    vec3 lighting = EvaluateDirectLight(normal, viewDirection, sunLightDirection, uSunRadiance.rgb,
        albedo, f0, metallic, roughness) * shadowFactor;

    for (int lightIndex = 0; lightIndex < 4; ++lightIndex)
    {
        vec3 offset = uPointPositions[lightIndex].xyz - fsIn.worldPosition;
        float distanceSquared = max(dot(offset, offset), 0.001);
        vec3 pointLightDirection = normalize(offset);
        vec3 pointRadiance = uPointRadiances[lightIndex].rgb / distanceSquared;
        lighting += EvaluateDirectLight(normal, viewDirection, pointLightDirection, pointRadiance,
            albedo, f0, metallic, roughness);
    }

    float normalDotView = max(dot(normal, viewDirection), 0.0);
    vec3 fresnelIbl = FresnelSchlickRoughness(normalDotView, f0, roughness);
    vec3 kS = fresnelIbl;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
    vec3 irradiance = texture(uIrradiance, normal).rgb;
    vec3 diffuse = irradiance * albedo;
    vec3 reflection = reflect(-viewDirection, normal);
    vec3 prefilteredColor = textureLod(uPrefilter, reflection, roughness * 4.0).rgb;
    vec2 brdf = texture(uBrdfLut, vec2(normalDotView, roughness)).rg;
    vec3 specular = prefilteredColor * (fresnelIbl * brdf.x + brdf.y);
    vec3 ambient = (kD * diffuse + specular) * ambientOcclusion;
    vec3 pbrColor = ambient + lighting;

    vec3 outputColor;
    if (uSettings.x == 2)
    {
        outputColor = pow(albedo, vec3(1.0 / 2.2));
    }
    else if (uSettings.x == 3)
    {
        outputColor = normal * 0.5 + 0.5;
    }
    else if (uSettings.x == 4)
    {
        outputColor = vec3(roughness, metallic, 0.0);
    }
    else if (uSettings.x == 5)
    {
        outputColor = vec3(shadowFactor);
    }
    else
    {
        vec3 toneMapped = pbrColor / (pbrColor + vec3(1.0));
        outputColor = pow(toneMapped, vec3(1.0 / 2.2));
    }
    FragColor = vec4(outputColor, 1.0);
}
