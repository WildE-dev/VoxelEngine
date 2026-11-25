#version 330 core
in vec2 uv;
in vec4 color;

uniform sampler2D tex;

out vec4 FragColor;

void main() {
    FragColor = color * texture(tex, uv);
}
