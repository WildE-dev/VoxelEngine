#version 330 core

layout (location = 0) in uint aData;
//layout (location = 1) in vec2 aTexCoord;
//layout (location = 2) in vec3 aNorm;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec2 TexCoord;
out float Light;

struct Data
{
  vec3 pos;
  vec2 tex;
  vec3 norm;
};

Data getData()
{
    float x = ((aData >> 24) & 0xFFu) / 8.0f;
    float y = ((aData >> 16) & 0xFFu) / 8.0f;
    float z = ((aData >> 8) & 0xFFu) / 8.0f;
    
    float u = (aData >> 7) & 0x1u;
    float v = (aData >> 6) & 0x1u;
    
    float nX = ((aData >> 4) & 0x3u) / 1.5f - 1.0f;
    float nY = ((aData >> 2) & 0x3u) / 1.5f - 1.0f;
    float nZ = (aData & 0x3u) / 1.5f - 1.0f;

    return Data(vec3(x, y, z), vec2(u, v), normalize(vec3(nX, nY, nZ)));
}

void main()
{
    Data data = getData();

    gl_Position = projection * view * model * vec4(data.pos, 1.0f);
    TexCoord = data.tex;

    float l = dot((model * vec4(data.norm, 1.0f)).xyz, vec3(0.0f, 1.0f, 0.0f));

    l = (l + 1.0f) / 2.0f;
    l = clamp(l + 0.12f, 0.0f, 1.0f);

    Light = l;
};