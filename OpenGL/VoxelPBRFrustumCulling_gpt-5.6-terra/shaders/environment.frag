#version 460 core

layout (location = 0) out vec4 FragColor;
in vec3 vLocalPosition;

void main()
{
    vec3 direction = normalize(vLocalPosition);
    vec3 radiance;
    if (direction.y >= 0.0)
    {
        float t = clamp(direction.y, 0.0, 1.0);
        radiance = mix(vec3(0.48, 0.576, 0.80), vec3(0.12, 0.27, 0.675), pow(t, 0.35));
    }
    else
    {
        radiance = vec3(0.035, 0.030, 0.025);
    }
    FragColor = vec4(radiance, 1.0);
}
