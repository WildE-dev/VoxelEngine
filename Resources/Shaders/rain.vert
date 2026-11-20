#version 330 core

layout (location = 0) in vec4 aDrop;   // xyz: position in the unit box, w: 0 bottom / 1 top
layout (location = 1) in float aChance; // shows while the intensity is above this

uniform mat4 view;
uniform mat4 projection;
uniform vec3 cameraPos;
uniform float time;
uniform float intensity;
uniform float radius;
uniform float height;
uniform float speed;
uniform float streak;

out float Alpha;

void main()
{
    if (aChance > intensity) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // off screen: clipped
        Alpha = 0.0;
        return;
    }
    // Fixed in the world, wrapped into a box around the camera: drops don't slide
    // along when the player moves, and the box always surrounds them.
    vec2 boxMin = cameraPos.xz - radius;
    vec2 xz = boxMin + mod(aDrop.xz * 2.0 * radius - boxMin, 2.0 * radius);
    float bottom = cameraPos.y - height * 0.5;
    float y = bottom + mod(aDrop.y * height - time * speed - bottom, height) + aDrop.w * streak;

    vec3 world = vec3(xz.x, y, xz.y);
    gl_Position = projection * view * vec4(world, 1.0);
    // Fade toward the edge of the box so it has no visible border.
    float d = length(xz - cameraPos.xz) / radius;
    Alpha = 0.75 * clamp(1.0 - d, 0.0, 1.0) * (0.5 + 0.5 * aDrop.w);
}
