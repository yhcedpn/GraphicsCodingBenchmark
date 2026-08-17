#version 460 core

layout(location = 0) in vec3 aPosition;
layout(location = 4) in vec4 aInstanceModel0;
layout(location = 5) in vec4 aInstanceModel1;
layout(location = 6) in vec4 aInstanceModel2;
layout(location = 7) in vec4 aInstanceModel3;

uniform mat4 uLightSpace;

void main() {
    mat4 model = mat4(aInstanceModel0, aInstanceModel1, aInstanceModel2, aInstanceModel3);
    gl_Position = uLightSpace * model * vec4(aPosition, 1.0);
}
