#version 330 core

in float Alpha;
uniform float brightness; // darker at night
uniform float snow;

out vec4 FragColor;

void main()
{
    FragColor = vec4(mix(vec3(0.82, 0.86, 0.95), vec3(1.0), snow) * brightness, Alpha);
}
