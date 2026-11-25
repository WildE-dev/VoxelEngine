#version 330 core
// The game's UI (src/ui/Ui.h): screen-space quads, textured and tinted.
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aUV;
layout (location = 2) in vec4 aColor;

uniform mat4 projection;

out vec2 uv;
out vec4 color;

void main() {
    uv = aUV;
    color = aColor;
    gl_Position = projection * vec4(aPos, 0.0, 1.0);
}
