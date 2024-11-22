#version 330 core

in vec2 TexCoord;
in float Light;

uniform sampler2D ourTexture;

out vec4 FragColor;

void main() {
    FragColor = Light * texture(ourTexture, TexCoord);
}