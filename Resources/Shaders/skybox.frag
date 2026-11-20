#version 330 core
out vec4 FragColor;

in vec3 TexCoords;

uniform samplerCube skybox;
uniform float brightness;
uniform vec3 tint;      // warm near sunrise and sunset
uniform float overcast; // 0..1: grey while it rains

void main()
{
    vec3 c = texture(skybox, TexCoords).rgb;
    float grey = dot(c, vec3(0.3, 0.59, 0.11));
    c = mix(c, vec3(grey * 0.75), overcast);
    FragColor = vec4(c * tint * brightness, 1.0);
}