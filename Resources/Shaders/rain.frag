#version 330 core

in float Alpha;
uniform float brightness; // darker at night

out vec4 FragColor;

void main()
{
    FragColor = vec4(vec3(0.82, 0.86, 0.95) * brightness, Alpha);
}
