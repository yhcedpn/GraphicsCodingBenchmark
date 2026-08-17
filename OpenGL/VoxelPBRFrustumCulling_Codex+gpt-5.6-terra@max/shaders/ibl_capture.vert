#version 460 core

layout (location = 0) in vec3 aPosition;

uniform mat4 uProjection;
uniform mat4 uView;

out vec3 vLocalPosition;

void main()
{
    vLocalPosition = aPosition;
    gl_Position = uProjection * uView * vec4(aPosition, 1.0);
}
