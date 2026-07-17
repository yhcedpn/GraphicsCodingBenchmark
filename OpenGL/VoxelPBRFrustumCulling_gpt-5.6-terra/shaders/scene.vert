#version 460 core

layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUv;
layout (location = 3) in mat4 aModel;
layout (location = 7) in uint aMaterialIndex;

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

out VS_OUT
{
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 uv;
    vec4 lightSpacePosition;
    flat uint materialIndex;
} vsOut;

void main()
{
    vec4 worldPosition = aModel * vec4(aPosition, 1.0);
    mat3 normalMatrix = mat3(transpose(inverse(aModel)));
    vsOut.worldPosition = worldPosition.xyz;
    vsOut.worldNormal = normalize(normalMatrix * aNormal);
    vsOut.uv = aUv;
    vsOut.lightSpacePosition = uLightSpace * worldPosition;
    vsOut.materialIndex = aMaterialIndex;
    gl_Position = uProjection * uView * worldPosition;
}
