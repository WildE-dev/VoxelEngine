#version 330 core

layout (location = 0) in uint aData1;
layout (location = 1) in uint aData2;
//layout (location = 2) in vec3 aNorm;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec2 TexCoord;
out float Light;

vec2 getTextureCoords(int textureIndex, int texturesPerRow, int textureSize, int atlasSize, float u, float v) {
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
    float x = ((aData1 >> 24) & 0xFFu) / 8.0f;
    float y = ((aData1 >> 16) & 0xFFu) / 8.0f;
    float z = ((aData1 >> 8) & 0xFFu) / 8.0f;
    
    float u = ((aData1 >> 4) & 0xFu) / 8.0f;
    float v = (aData1 & 0xFu) / 8.0f;

    int textureIndex = int(aData2 & 0xFFFFFu);
    
    vec2 coords = getTextureCoords(textureIndex, 2, 16, 32, u, v);

    float nX = ((aData2 >> 28) & 0xFu) / 7.5f - 1.0f;
    float nY = ((aData2 >> 24) & 0xFu) / 7.5f - 1.0f;
    float nZ = ((aData2 >> 20) & 0xFu) / 7.5f - 1.0f;

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