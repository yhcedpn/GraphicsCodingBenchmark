#version 460 core

layout (location = 0) out vec4 FragColor;
in vec2 vUv;

uniform sampler2D uSceneColor;

void main()
{
    FragColor = texture(uSceneColor, vUv);
}
