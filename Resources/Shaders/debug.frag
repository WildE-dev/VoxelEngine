#version 330 core

uniform vec4 color; // (black: the outline of the block in view)

out vec4 FragColor;

void main() {
    FragColor = color;
}