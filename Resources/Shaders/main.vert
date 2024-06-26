#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec2 aTexCoord;
layout (location = 2) in vec3 aNorm;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec2 TexCoord;
out float Light;

void main()
{
    gl_Position = projection * view * model * vec4(aPos, 1.0f);
    TexCoord = aTexCoord;

    float l = dot((model * vec4(aNorm, 1.0f)).xyz, vec3(0.0f, 1.0f, 0.0f));

    l = (l + 1.0f) / 2.0f;
    l = clamp(l + 0.12f, 0.0f, 1.0f);

    Light = l;
};