#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec3 normal;
out vec3 WorldPos;
out vec2 faceUV; // where on its face this is (0..1), for a textured box
// A model's textured box tiled by its size (its texture repeating once a block):
// the box's size; zero for one tile stretched over each face.
uniform vec3 boxRepeat;

void main()
{
    normal = mat3(model) * aNormal;
    // The unit cube's face coordinates, upright on the sides.
    vec3 p = aPos + 0.5;
    if (abs(aNormal.x) > 0.5) faceUV = vec2(aNormal.x > 0.0 ? 1.0 - p.z : p.z, 1.0 - p.y);
    else if (abs(aNormal.z) > 0.5) faceUV = vec2(aNormal.z > 0.0 ? p.x : 1.0 - p.x, 1.0 - p.y);
    else faceUV = p.xz;
    if (boxRepeat.x > 0.0) {
        if (abs(aNormal.x) > 0.5) faceUV *= boxRepeat.zy;
        else if (abs(aNormal.z) > 0.5) faceUV *= boxRepeat.xy;
        else faceUV *= boxRepeat.xz;
    }
    vec4 world = model * vec4(aPos, 1.0);
    WorldPos = world.xyz;
    gl_Position = projection * view * world;
}
