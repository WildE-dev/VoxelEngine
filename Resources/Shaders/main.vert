#version 330 core

layout (location = 0) in uint aData1;
layout (location = 1) in uint aData2;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform float aoStrength;     // 0 = off, 1 = full
uniform float sunBrightness;  // 0 = night, 1 = day
uniform vec3 blockLightColor; // colour of light from emitting blocks
// Lights carried in hand (a lamp, a campfire): where (xyz) and how strong (w, a light
// level), falling off a level a block like placed light.
uniform int handLightCount;
uniform vec4 handLights[8];

out vec2 TexCoord;
out vec3 Light;
out vec3 WorldPos;

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

// The vertex's two words: see src/world/MeshVertex.h (positions in sixteenths).
vec3 decodeNormal(uint bits)
{
    vec2 f = vec2(float((bits >> 4) & 0xFu), float(bits & 0xFu)) / 7.0 - 1.0;
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(f.yx)) * vec2(f.x >= 0.0 ? 1.0 : -1.0, f.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

Data getData()
{
    float x = float((aData1 >> 23) & 0x1FFu) / 16.0f;
    float y = float((aData1 >> 14) & 0x1FFu) / 16.0f - 1.0f; // stored one block up
    float z = float((aData1 >> 5) & 0x1FFu) / 16.0f;

    float u = float(aData1 & 0x1Fu) / 16.0f;
    float v = float((aData2 >> 27) & 0x1Fu) / 16.0f;

    int textureIndex = int(aData2 & 0x1FFu);
    float blockLight = float((aData2 >> 13) & 0xFu);
    float skyLight = float((aData2 >> 9) & 0xFu);
    float ao = float((aData2 >> 17) & 0x3u) / 3.0f; // (water is drawn by water.vert)

    vec2 coords = getTextureCoords(textureIndex, u, v);

    return Data(vec3(x, y, z), coords, decodeNormal((aData2 >> 19) & 0xFFu), ao, blockLight, skyLight);
}

void main()
{
    Data data = getData();

    vec4 world = model * vec4(data.pos, 1.0f);
    WorldPos = world.xyz;
    gl_Position = projection * view * world;
    TexCoord = data.tex;

    // Light levels: each is ~85% as bright as the one above, level 0 is dark.
    float sky = data.skyLight > 0.0f ? pow(0.85f, 15.0f - data.skyLight) : 0.0f;
    float block = data.blockLight > 0.0f ? pow(0.85f, 15.0f - data.blockLight) : 0.0f;
    for (int i = 0; i < handLightCount; i++) {
        float level = handLights[i].w - distance(WorldPos, handLights[i].xyz);
        if (level > 0.0f) block = max(block, pow(0.85f, 15.0f - level));
    }

    // Sky: brightest on faces turned up toward it, half on walls. Faces turned down
    // still see the bright sky and ground around them (skylight bounces), so the
    // underside of an overhang gets 40% instead of going black. Only where the sky
    // light reaches (the sky level is sampled in the cell the face looks into).
    // Directions must ignore the model translation (w = 0).
    vec3 normal = normalize(mat3(model) * data.norm);
    float up = normal.y;
    float facing = up >= 0.0f ? 0.5f + 0.5f * up : 0.5f + 0.1f * up;
    float sun = facing * sky * sunBrightness;

    // A little ambient so unlit areas at night aren't pure black.
    vec3 light = vec3(0.04f) + vec3(sun) + block * blockLightColor;

    // Ambient occlusion: fully occluded corners drop to 40% brightness.
    light *= mix(1.0f, mix(0.4f, 1.0f, data.ao), aoStrength);

    Light = min(light, vec3(1.0f));
}
