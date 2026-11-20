#version 330 core

layout (location = 0) in uint aData1;
layout (location = 1) in uint aData2;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float aoStrength;     // 0 = off, 1 = full
uniform float sunBrightness;  // 0 = night, 1 = day
uniform vec3 blockLightColor; // colour of light from emitting blocks
uniform float waterPass;      // 1 while drawing translucent water
uniform float time;           // seconds, for the water shimmer

out vec2 TexCoord;
out vec3 Light;

// Atlas layout, set by TextureAtlas::ApplyUniforms.
uniform int atlasColumns;
uniform vec2 atlasCellUV;    // size of one padded cell
uniform vec2 atlasPaddingUV; // gutter around each tile
uniform vec2 atlasTileUV;    // size of the tile inside a cell

vec2 getTextureCoords(int textureIndex, float u, float v) {
    vec2 cell = vec2(textureIndex % atlasColumns, textureIndex / atlasColumns);
    return cell * atlasCellUV + atlasPaddingUV + vec2(u, v) * atlasTileUV;
}

struct Data
{
  vec3 pos;
  vec2 tex;
  vec3 norm;
  float ao;
  float blockLight;
  float skyLight;
};

Data getData()
{
    float x = ((aData1 >> 24) & 0xFFu) / 8.0f;
    float y = ((aData1 >> 16) & 0xFFu) / 8.0f - 1.0f; // stored one block up (see Block.cpp)
    float z = ((aData1 >> 8) & 0xFFu) / 8.0f;
    
    float u = ((aData1 >> 4) & 0xFu) / 8.0f;
    float v = (aData1 & 0xFu) / 8.0f;

    int textureIndex = int(aData2 & 0x3FFu);
    float blockLight = float((aData2 >> 14) & 0xFu);
    float skyLight = float((aData2 >> 10) & 0xFu);
    // Water has no AO; its two AO bits are extra surface height in 32nds of a block.
    float ao = float((aData2 >> 18) & 0x3u) / 3.0f;
    if (waterPass > 0.5f) {
        y += float((aData2 >> 18) & 0x3u) / 32.0f;
        ao = 1.0f;
    }
    
    vec2 coords = getTextureCoords(textureIndex, u, v);

    float nX = ((aData2 >> 28) & 0xFu) / 7.5f - 1.0f;
    float nY = ((aData2 >> 24) & 0xFu) / 7.5f - 1.0f;
    float nZ = ((aData2 >> 20) & 0xFu) / 7.5f - 1.0f;

    return Data(vec3(x, y, z), coords, normalize(vec3(nX, nY, nZ)), ao, blockLight, skyLight);
}

void main()
{
    Data data = getData();

    gl_Position = projection * view * model * vec4(data.pos, 1.0f);
    TexCoord = data.tex;

    // Light levels: each is ~85% as bright as the one above, level 0 is dark.
    float sky = data.skyLight > 0.0f ? pow(0.85f, 15.0f - data.skyLight) : 0.0f;
    float block = data.blockLight > 0.0f ? pow(0.85f, 15.0f - data.blockLight) : 0.0f;

    // Sun: simple directional term from straight above, only where the sky reaches.
    // Directions must ignore the model translation (w = 0).
    float sun = dot(normalize(mat3(model) * data.norm), vec3(0.0f, 1.0f, 0.0f));
    sun = clamp((sun + 1.0f) / 2.0f, 0.0f, 1.0f) * sky * sunBrightness;

    // A little ambient so unlit areas at night aren't pure black.
    vec3 light = vec3(0.04f) + vec3(sun) + block * blockLightColor;

    // Ambient occlusion: fully occluded corners drop to 40% brightness.
    light *= mix(1.0f, mix(0.4f, 1.0f, data.ao), aoStrength);

    // Water surfaces shimmer gently.
    if (waterPass > 0.5f) {
        vec3 world = (model * vec4(data.pos, 1.0f)).xyz;
        light *= 0.92f + 0.08f * sin(time * 1.8f + world.x * 0.9f + world.z * 0.7f);
    }

    Light = min(light, vec3(1.0f));
}
