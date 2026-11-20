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

void main()
{
    float x = float((aData1 >> 24) & 0xFFu) / 8.0;
    float y = float((aData1 >> 16) & 0xFFu) / 8.0 - 1.0 - float((aData2 >> 18) & 0x3u) / 32.0;
    float z = float((aData1 >> 8) & 0xFFu) / 8.0;

    float nX = float((aData2 >> 28) & 0xFu) / 7.5 - 1.0;
    float nY = float((aData2 >> 24) & 0xFu) / 7.5 - 1.0;
    float nZ = float((aData2 >> 20) & 0xFu) / 7.5 - 1.0;

    float blockLevel = float((aData2 >> 14) & 0xFu);
    float skyLevel = float((aData2 >> 10) & 0xFu);
    // Each light level is ~85% as bright as the one above; 0 is dark.
    BlockLight = blockLevel > 0.0 ? pow(0.85, 15.0 - blockLevel) : 0.0;
    SkyLight = skyLevel > 0.0 ? pow(0.85, 15.0 - skyLevel) : 0.0;

    int textureIndex = int(aData2 & 0x3FFu);
    vec2 cell = vec2(textureIndex % atlasColumns, textureIndex / atlasColumns);
    vec2 uv = vec2(float((aData1 >> 4) & 0xFu), float(aData1 & 0xFu)) / 8.0;
    TexCoord = cell * atlasCellUV + atlasPaddingUV + uv * atlasTileUV;

    vec4 world = model * vec4(x, y, z, 1.0);
    WorldPos = world.xyz;
    FaceNormal = normalize(mat3(model) * vec3(nX, nY, nZ));
    gl_Position = projection * view * world;
}
