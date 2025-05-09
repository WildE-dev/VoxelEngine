#version 330 core

in vec2 texCoord;

uniform sampler2D screenTexture;

out vec4 FragColor;

void main() {
    FragColor = texture(screenTexture, texCoord);
}