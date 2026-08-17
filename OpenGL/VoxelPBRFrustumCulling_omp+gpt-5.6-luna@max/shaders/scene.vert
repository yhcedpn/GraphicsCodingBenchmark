#version 460 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec3 aTangent;
layout(location = 4) in vec4 aInstanceModel0;
layout(location = 5) in vec4 aInstanceModel1;
layout(location = 6) in vec4 aInstanceModel2;
layout(location = 7) in vec4 aInstanceModel3;
layout(location = 8) in int aMaterialIndex;

layout(std140, binding = 0) uniform FrameBlock {
    mat4 uViewProjection;
    mat4 uLightSpace;
    vec4 uCameraPosition;
    vec4 uSunDirection;
    vec4 uSunRadiance;
    vec4 uPointPositions[4];
    vec4 uPointColors[4];
};

out vec3 vWorldPosition;
out vec3 vWorldNormal;
out vec3 vWorldTangent;
out vec3 vWorldBitangent;
out vec2 vUV;
out vec4 vLightSpacePosition;
flat out int vMaterialIndex;

void main() {
    mat4 model = mat4(aInstanceModel0, aInstanceModel1, aInstanceModel2, aInstanceModel3);
    vec4 worldPosition = model * vec4(aPosition, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(model)));
    vec3 worldNormal = normalize(normalMatrix * aNormal);
    vec3 worldTangent = normalize(mat3(model) * aTangent);
    vec3 worldBitangent = normalize(cross(worldNormal, worldTangent));

    vWorldPosition = worldPosition.xyz;
    vWorldNormal = worldNormal;
    vWorldTangent = worldTangent;
    vWorldBitangent = worldBitangent;
    vUV = aUV;
    vLightSpacePosition = uLightSpace * worldPosition;
    vMaterialIndex = aMaterialIndex;
    gl_Position = uViewProjection * worldPosition;
}
