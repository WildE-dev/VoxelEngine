#version 330 core

layout (location = 0) in uint aData;
layout (location = 1) in uint aNorm;
//layout (location = 2) in vec3 aNorm;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec2 TexCoord;
out float Light;

vec2 getTextureCoords(int textureIndex, int texturesPerRow, int textureSize, int atlasSize, int u, int v) {
    int x = textureIndex % texturesPerRow;
    int y = textureIndex / texturesPerRow;
    
    float uMin = float(x * textureSize) / atlasSize;
    float uMax = uMin + float(textureSize) / atlasSize;
    
    float vMin = float(y * textureSize) / atlasSize;
    float vMax = vMin + float(textureSize) / atlasSize;

    return vec2((uMin * (1 - u)) + (uMax * u), (vMin * (1 - v)) + (vMax * v));
}

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
    
    int u = int((aData >> 7) & 0x1u);
    int v = int((aData >> 6) & 0x1u);

    int textureIndex = int(aData & 0x3Fu);
    
    vec2 coords = getTextureCoords(textureIndex, 2, 16, 32, u, v);

    float nX = ((aNorm >> 28) & 0xFu) / 7.5f - 1.0f;
    float nY = ((aNorm >> 24) & 0xFu) / 7.5f - 1.0f;
    float nZ = ((aNorm >> 20) & 0xFu) / 7.5f - 1.0f;

    return Data(vec3(x, y, z), coords, normalize(vec3(nX, nY, nZ)));
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