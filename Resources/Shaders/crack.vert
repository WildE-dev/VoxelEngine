#version 330 core
// The box around a block being mined (the skybox's unit cube, -0.5..0.5), a hair
// bigger than the block so the cracks sit on its faces.
layout (location = 0) in vec3 aPos;

uniform mat4 view;
uniform mat4 projection;
uniform vec3 boxMin;
uniform vec3 boxSize;

out vec3 Local; // 0..1 across the box

void main() {
    Local = aPos + 0.5;
    vec3 world = boxMin - 0.004 + Local * (boxSize + 0.008);
    gl_Position = projection * view * vec4(world, 1.0);
}
