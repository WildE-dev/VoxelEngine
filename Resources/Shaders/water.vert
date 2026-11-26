#version 330 core

// Water surfaces. Same packed vertex format as main.vert (see Block.cpp), except that
// the two AO bits hold how many 32nds of a block the surface sits below the vertex's
// (rounded up) height.
layout (location = 0) in uint aData1;
layout (location = 1) in uint aData2;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out vec3 WorldPos;
out vec3 FaceNormal;
out float SkyLight;   // 0..1
out float BlockLight; // 0..1
out vec2 TexCoord;

// Atlas layout, set by TextureAtlas::ApplyUniforms.
uniform int atlasColumns;
uniform vec2 atlasCellUV;
uniform vec2 atlasPaddingUV;
uniform vec2 atlasTileUV;

// The vertex's two words: see src/world/MeshVertex.h (positions in sixteenths).
vec3 decodeNormal(uint bits)
{
    vec2 f = vec2(float((bits >> 4) & 0xFu), float(bits & 0xFu)) / 7.0 - 1.0;
    vec3 n = vec3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(f.yx)) * vec2(f.x >= 0.0 ? 1.0 : -1.0, f.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

void main()
{
    float x = float((aData1 >> 23) & 0x1FFu) / 16.0;
    float y = float((aData1 >> 14) & 0x1FFu) / 16.0 - 1.0 - float((aData2 >> 17) & 0x3u) / 32.0;
    float z = float((aData1 >> 5) & 0x1FFu) / 16.0;

    vec3 normal = decodeNormal((aData2 >> 19) & 0xFFu);

    float blockLevel = float((aData2 >> 13) & 0xFu);
    float skyLevel = float((aData2 >> 9) & 0xFu);
    // Each light level is ~85% as bright as the one above; 0 is dark.
    BlockLight = blockLevel > 0.0 ? pow(0.85, 15.0 - blockLevel) : 0.0;
    SkyLight = skyLevel > 0.0 ? pow(0.85, 15.0 - skyLevel) : 0.0;

    int textureIndex = int(aData2 & 0x1FFu);
    vec2 cell = vec2(textureIndex % atlasColumns, textureIndex / atlasColumns);
    vec2 uv = vec2(float(aData1 & 0x1Fu), float((aData2 >> 27) & 0x1Fu)) / 16.0;
    TexCoord = cell * atlasCellUV + atlasPaddingUV + uv * atlasTileUV;

    vec4 world = model * vec4(x, y, z, 1.0);
    WorldPos = world.xyz;
    FaceNormal = normalize(mat3(model) * normal);
    gl_Position = projection * view * world;
}
