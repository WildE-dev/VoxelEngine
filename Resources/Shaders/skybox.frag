#version 330 core
out vec4 FragColor;

in vec3 TexCoords;

uniform samplerCube skybox;
uniform float brightness;

void main()
{    
    FragColor = vec4(texture(skybox, TexCoords).rgb * brightness, 1.0);
}