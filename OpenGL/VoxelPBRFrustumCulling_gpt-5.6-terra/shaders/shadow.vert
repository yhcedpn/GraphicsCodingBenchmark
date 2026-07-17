#version 460 core

layout (location = 0) in vec3 aPosition;
layout (location = 3) in mat4 aModel;

uniform mat4 uLightSpace;

void main()
{
    gl_Position = uLightSpace * aModel * vec4(aPosition, 1.0);
}
