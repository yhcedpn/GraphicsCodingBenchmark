#version 460 core

layout(location = 0) out vec4 FragColor;
layout(binding = 0) uniform sampler2D uSceneColor;
in vec2 vUV;

void main() {
    FragColor = texture(uSceneColor, vUV);
}
