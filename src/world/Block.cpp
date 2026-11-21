#include "world/Block.h"

#include <algorithm>
#include <array>
#include <atomic>

namespace {

// Unpacked vertex, packed into two uint32s for upload. See main.vert for the decode side.
struct VertexData {
    glm::vec3 pos{0.0f};
    glm::vec2 tex{0.0f};
    int textureIndex = 0;
    glm::vec3 norm{0.0f};
    uint32_t ao = 3;
    uint32_t light = 0;
    uint32_t sky = 0;

    VertexData() = default;
    VertexData(glm::vec3 pos, glm::vec2 tex, int textureIndex, glm::vec3 norm)
        : pos(pos), tex(tex), textureIndex(textureIndex), norm(norm) {}

    // xyz in 1/8 block units (8 bits each), uv in 1/8 units (4 bits each). y is
    // stored one block up so plant sprites can reach below their chunk's floor.
    uint32_t PackPosition() const {
        uint32_t x = static_cast<uint32_t>(pos.x * 8);
        uint32_t y = static_cast<uint32_t>((pos.y + 1.0f) * 8);
        uint32_t z = static_cast<uint32_t>(pos.z * 8);
        uint32_t u = static_cast<uint32_t>(tex.x * 8);
        uint32_t v = static_cast<uint32_t>(tex.y * 8);
        return ((x & 0xFFu) << 24) | ((y & 0xFFu) << 16) | ((z & 0xFFu) << 8) | ((u & 0xFu) << 4) | (v & 0xFu);
    }

    // Normal quantised to 4 bits per axis, 2-bit ambient occlusion, 4-bit block
    // light, 4-bit sky light, 10-bit texture index.
    uint32_t PackShading() const {
        uint32_t nx = static_cast<uint32_t>((norm.x + 1.0f) * 7.5f);
        uint32_t ny = static_cast<uint32_t>((norm.y + 1.0f) * 7.5f);
        uint32_t nz = static_cast<uint32_t>((norm.z + 1.0f) * 7.5f);
        return ((nx & 0xFu) << 28) | ((ny & 0xFu) << 24) | ((nz & 0xFu) << 20) |
               ((ao & 0x3u) << 18) | ((light & 0xFu) << 14) | ((sky & 0xFu) << 10) |
               (static_cast<uint32_t>(textureIndex) & 0x3FFu);
    }
};

glm::vec3 GetNormal(glm::vec3 p1, glm::vec3 p2, glm::vec3 p3) {
    glm::vec3 A = p3 - p1;
    glm::vec3 B = p2 - p1;
    glm::vec3 cross = glm::normalize(glm::cross(A, B));
    return cross;
}

std::array<VertexData, 6> GetFaceVertices(Face face, const EdgeData& edgeData, int x, int y, int z, const TextureData& textureData) {
    std::array<VertexData, 6> vertices;

    float bottomY0 = static_cast<float>(edgeData.GetBottomY(0)) / 8.0f;
    float bottomY1 = static_cast<float>(edgeData.GetBottomY(1)) / 8.0f;
    float bottomY2 = static_cast<float>(edgeData.GetBottomY(2)) / 8.0f;
    float bottomY3 = static_cast<float>(edgeData.GetBottomY(3)) / 8.0f;

    float topY0 = static_cast<float>(edgeData.GetTopY(0)) / 8.0f;
    float topY1 = static_cast<float>(edgeData.GetTopY(1)) / 8.0f;
    float topY2 = static_cast<float>(edgeData.GetTopY(2)) / 8.0f;
    float topY3 = static_cast<float>(edgeData.GetTopY(3)) / 8.0f;

    switch (face) {
    case Face::Right:
    {
        glm::vec3 pos1 = glm::vec3(x + 1.0f, y + bottomY1, z);
        glm::vec3 pos2 = glm::vec3(x + 1.0f, y + bottomY3, z + 1.0f);
        glm::vec3 pos3 = glm::vec3(x + 1.0f, y + topY1, z);
        glm::vec3 pos4 = glm::vec3(x + 1.0f, y + topY3, z + 1.0f);

        glm::vec2 tex1 = glm::vec2(1.0f, topY1);
        glm::vec2 tex2 = glm::vec2(0.0f, topY3);
        glm::vec2 tex3 = glm::vec2(1.0f, bottomY1);
        glm::vec2 tex4 = glm::vec2(0.0f, bottomY3);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.right, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.right, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.right, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.right, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.right, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.right, norm2);
        break;
    }
    case Face::Left:
    {
        glm::vec3 pos1 = glm::vec3(x, y + bottomY0, z + 1.0f);
        glm::vec3 pos2 = glm::vec3(x, y + bottomY2, z);
        glm::vec3 pos3 = glm::vec3(x, y + topY0, z + 1.0f);
        glm::vec3 pos4 = glm::vec3(x, y + topY2, z);

        glm::vec2 tex1 = glm::vec2(1.0f, topY0);
        glm::vec2 tex2 = glm::vec2(0.0f, topY2);
        glm::vec2 tex3 = glm::vec2(1.0f, bottomY0);
        glm::vec2 tex4 = glm::vec2(0.0f, bottomY2);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.left, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.left, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.left, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.left, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.left, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.left, norm2);
        break;
    }
    case Face::Top:
    {
        glm::vec3 pos1 = glm::vec3(x + 1.0f, y + topY3, z + 1.0f);
        glm::vec3 pos2 = glm::vec3(x, y + topY0, z + 1.0f);
        glm::vec3 pos3 = glm::vec3(x + 1.0f, y + topY1, z);
        glm::vec3 pos4 = glm::vec3(x, y + topY2, z);

        glm::vec2 tex1 = glm::vec2(1.0f, 1.0f);
        glm::vec2 tex2 = glm::vec2(0.0f, 1.0f);
        glm::vec2 tex3 = glm::vec2(1.0f, 0.0f);
        glm::vec2 tex4 = glm::vec2(0.0f, 0.0f);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.top, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.top, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.top, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.top, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.top, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.top, norm2);
        break;
    }
    case Face::Bottom:
    {
        glm::vec3 pos1 = glm::vec3(x, y + bottomY2, z);
        glm::vec3 pos2 = glm::vec3(x, y + bottomY0, z + 1.0f);
        glm::vec3 pos3 = glm::vec3(x + 1.0f, y + bottomY1, z);
        glm::vec3 pos4 = glm::vec3(x + 1.0f, y + bottomY3, z + 1.0f);

        glm::vec2 tex1 = glm::vec2(1.0f, 1.0f);
        glm::vec2 tex2 = glm::vec2(0.0f, 1.0f);
        glm::vec2 tex3 = glm::vec2(1.0f, 0.0f);
        glm::vec2 tex4 = glm::vec2(0.0f, 0.0f);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.bottom, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.bottom, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.bottom, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.bottom, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.bottom, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.bottom, norm2);
        break;
    }
    case Face::Back:
    {
        glm::vec3 pos1 = glm::vec3(x, y + bottomY0, z + 1.0f);
        glm::vec3 pos2 = glm::vec3(x, y + topY0, z + 1.0f);
        glm::vec3 pos3 = glm::vec3(x + 1.0f, y + bottomY3, z + 1.0f);
        glm::vec3 pos4 = glm::vec3(x + 1.0f, y + topY3, z + 1.0f);

        glm::vec2 tex1 = glm::vec2(0.0f, topY0);
        glm::vec2 tex2 = glm::vec2(0.0f, bottomY0);
        glm::vec2 tex3 = glm::vec2(1.0f, topY3);
        glm::vec2 tex4 = glm::vec2(1.0f, bottomY3);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.back, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.back, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.back, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.back, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.back, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.back, norm2);
        break;
    }
    case Face::Front:
    {
        glm::vec3 pos1 = glm::vec3(x + 1.0f, y + bottomY1, z);
        glm::vec3 pos2 = glm::vec3(x + 1.0f, y + topY1, z);
        glm::vec3 pos3 = glm::vec3(x, y + bottomY2, z);
        glm::vec3 pos4 = glm::vec3(x, y + topY2, z);

        glm::vec2 tex1 = glm::vec2(0.0f, topY1);
        glm::vec2 tex2 = glm::vec2(0.0f, bottomY1);
        glm::vec2 tex3 = glm::vec2(1.0f, topY2);
        glm::vec2 tex4 = glm::vec2(1.0f, bottomY2);

        glm::vec3 norm1 = GetNormal(pos1, pos2, pos3);
        glm::vec3 norm2 = GetNormal(pos3, pos2, pos4);

        vertices[0] = VertexData(pos1, tex1, textureData.front, norm1);
        vertices[1] = VertexData(pos2, tex2, textureData.front, norm1);
        vertices[2] = VertexData(pos3, tex3, textureData.front, norm1);

        vertices[3] = VertexData(pos3, tex3, textureData.front, norm2);
        vertices[4] = VertexData(pos2, tex2, textureData.front, norm2);
        vertices[5] = VertexData(pos4, tex4, textureData.front, norm2);
        break;
    }
    }

    return vertices;
}

} // namespace

namespace {

// Texture file names (without extension) in Resources/Textures/Blocks, per face:
// top, bottom, front, back, left, right.
struct BlockTextureNames {
    const char* faces[6];
};

constexpr int kBlockTypeCount = static_cast<int>(BlockType::COUNT);

const BlockTextureNames kBlockTextureNames[kBlockTypeCount] = {
    /* AIR   */ {{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* GRASS */ {{ "grass_top", "dirt", "grass_side", "grass_side", "grass_side", "grass_side" }},
    /* DIRT  */ {{ "dirt", "dirt", "dirt", "dirt", "dirt", "dirt" }},
    /* STONE */ {{ "stone", "stone", "stone", "stone", "stone", "stone" }},
    /* LAMP  */ {{ "lamp", "lamp", "lamp", "lamp", "lamp", "lamp" }},
    /* LOG   */ {{ "log_top", "log_top", "log_side", "log_side", "log_side", "log_side" }},
    /* LEAVES*/ {{ "leaves", "leaves", "leaves", "leaves", "leaves", "leaves" }},
    /* TALL_GRASS    */ {{ "tall_grass", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* FLOWER_RED    */ {{ "flower_red", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* FLOWER_YELLOW */ {{ "flower_yellow", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* SAND  */ {{ "sand", "sand", "sand", "sand", "sand", "sand" }},
    /* WATER */ {{ "water", "water", "water", "water", "water", "water" }},
    /* PUMP  */ {{ "pump_top", "pump_top", "pump_side", "pump_side", "pump_side", "pump_side" }},
    /* PIPE  */ {{ "pipe_end", "pipe_end", "pipe_end", "pipe_end", "pipe_end", "pipe_end" }},
    /* STONE_BRICK */ {{ "stone_brick", "stone_brick", "stone_brick", "stone_brick", "stone_brick", "stone_brick" }},
    /* MOSSY_BRICK */ {{ "mossy_brick", "mossy_brick", "mossy_brick", "mossy_brick", "mossy_brick", "mossy_brick" }},
    /* SPIKES */ {{ "spikes", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* CRUMBLING_BRICK */ {{ "cracked_brick", "cracked_brick", "cracked_brick", "cracked_brick", "cracked_brick", "cracked_brick" }},
    /* PRESSURE_PLATE */ {{ "pressure_plate", "pressure_plate", "pressure_plate", "pressure_plate", "pressure_plate", "pressure_plate" }},
    /* SLUICE */ {{ "sluice", "sluice", "sluice", "sluice", "sluice", "sluice" }},
    /* SNOW  */ {{ "snow", "stone", "snow_side", "snow_side", "snow_side", "snow_side" }},
    /* DRY_GRASS   */ {{ "dry_grass_top", "dirt", "dry_grass_side", "dry_grass_side", "dry_grass_side", "dry_grass_side" }},
    /* LUSH_GRASS  */ {{ "lush_grass_top", "dirt", "lush_grass_side", "lush_grass_side", "lush_grass_side", "lush_grass_side" }},
    /* SNOWY_GRASS */ {{ "snowy_grass_top", "dirt", "snowy_grass_side", "snowy_grass_side", "snowy_grass_side", "snowy_grass_side" }},
    /* PINE_LEAVES */ {{ "pine_leaves", "pine_leaves", "pine_leaves", "pine_leaves", "pine_leaves", "pine_leaves" }},
    /* CACTUS */ {{ "cactus_top", "cactus_top", "cactus_side", "cactus_side", "cactus_side", "cactus_side" }},
    /* DEAD_BUSH */ {{ "dead_bush", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* ICE   */ {{ "ice", "ice", "ice", "ice", "ice", "ice" }},
    /* PEBBLES */ {{ "pebbles", "stone", nullptr, nullptr, nullptr, nullptr }}, // icon; the stones' own texture
    /* PLANKS */ {{ "planks", "planks", "planks", "planks", "planks", "planks" }},
    /* GLASS  */ {{ "glass", "glass", "glass", "glass", "glass", "glass" }},
    /* RAW_VENISON    */ {{ "raw_venison", "raw_venison", "raw_venison", "raw_venison", "raw_venison", "raw_venison" }}, // items: their icon
    /* COOKED_VENISON */ {{ "cooked_venison", "cooked_venison", "cooked_venison", "cooked_venison", "cooked_venison", "cooked_venison" }},
    /* HIDE           */ {{ "hide", "hide", "hide", "hide", "hide", "hide" }},
    /* CHEST */ {{ "chest_top", "chest_top", "chest_side", "chest_side", "chest_side", "chest_side" }},
    /* BED   */ {{ "bed_top", "planks", "bed_side", "bed_side", "bed_side", "bed_side" }},
    /* COAL_ORE   */ {{ "coal_ore", "coal_ore", "coal_ore", "coal_ore", "coal_ore", "coal_ore" }},
    /* IRON_ORE   */ {{ "iron_ore", "iron_ore", "iron_ore", "iron_ore", "iron_ore", "iron_ore" }},
    /* COAL       */ {{ "coal", "coal", "coal", "coal", "coal", "coal" }},
    /* IRON_INGOT */ {{ "iron_ingot", "iron_ingot", "iron_ingot", "iron_ingot", "iron_ingot", "iron_ingot" }},
    /* SEEDS      */ {{ "seeds", "seeds", "seeds", "seeds", "seeds", "seeds" }},
    /* CROP       */ {{ "crop", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* RIPE_WHEAT */ {{ "ripe_wheat", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* WHEAT      */ {{ "wheat", "wheat", "wheat", "wheat", "wheat", "wheat" }},
    /* BREAD      */ {{ "bread", "bread", "bread", "bread", "bread", "bread" }},
    /* LADDER     */ {{ "ladder", "ladder", "ladder", "ladder", "ladder", "ladder" }},
    /* DOOR       */ {{ "planks", "planks", "door", "door", "door", "door" }},
    /* DOOR_OPEN  */ {{ "door_open", "door_open", "door_open", "door_open", "door_open", "door_open" }},
    /* ARROW      */ {{ "arrow", "arrow", "arrow", "arrow", "arrow", "arrow" }},
    /* RAW_FISH    */ {{ "raw_fish", "raw_fish", "raw_fish", "raw_fish", "raw_fish", "raw_fish" }},
    /* COOKED_FISH */ {{ "cooked_fish", "cooked_fish", "cooked_fish", "cooked_fish", "cooked_fish", "cooked_fish" }},
    /* CAMPFIRE    */ {{ "campfire", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* SIGN        */ {{ "sign", nullptr, nullptr, nullptr, nullptr, nullptr }},
    /* TNT         */ {{ "tnt_top", "tnt_top", "tnt_side", "tnt_side", "tnt_side", "tnt_side" }},
    /* TNT_LIT     */ {{ "tnt_lit", "tnt_lit", "tnt_lit", "tnt_lit", "tnt_lit", "tnt_lit" }},
    /* BOAT        */ {{ "boat", "boat", "boat", "boat", "boat", "boat" }},
};

// What a block shows where it's been carved into (chiselled faces inside its cell),
// for blocks whose inside differs from their outside: a log's wood under the bark.
const char* CarvedTextureName(BlockType type) {
    return type == BlockType::LOG ? "log_inside" : nullptr;
}

// Resolved atlas indices. Written on the main thread on (re)load, read by the
// mesher on the worker thread, hence atomic.
std::atomic<int> gBlockTextureIndices[kBlockTypeCount][6] = {};
std::atomic<int> gCarvedTextureIndices[kBlockTypeCount] = {}; // -1: none

} // namespace

void ResolveBlockTextures(const std::function<int(const std::string&)>& lookup) {
    for (int type = 0; type < kBlockTypeCount; type++) {
        for (int face = 0; face < 6; face++) {
            const char* name = kBlockTextureNames[type].faces[face];
            gBlockTextureIndices[type][face] = name ? lookup(name) : 0;
        }
        const char* carved = CarvedTextureName(static_cast<BlockType>(type));
        gCarvedTextureIndices[type] = carved ? lookup(carved) : -1;
    }
}

namespace {
constexpr const char* kBlockIds[kBlockTypeCount] = {
    "air", "grass", "dirt", "stone", "lamp", "log", "leaves", "tall_grass", "flower_red", "flower_yellow", "sand", "water", "pump", "pipe",
    "stone_brick", "mossy_brick", "spikes", "crumbling_brick", "pressure_plate", "sluice", "snow", "dry_grass", "lush_grass", "snowy_grass",
    "pine_leaves", "cactus", "dead_bush", "ice", "pebbles", "planks", "glass", "raw_venison", "cooked_venison", "hide", "chest", "bed", "coal_ore", "iron_ore", "coal", "iron_ingot", "seeds", "crop", "ripe_wheat", "wheat", "bread", "ladder", "door", "door_open", "arrow", "raw_fish", "cooked_fish", "campfire", "sign", "tnt", "tnt_lit", "boat",
};
} // namespace

const char* BlockId(BlockType type) {
    const int t = static_cast<int>(type);
    return t >= 0 && t < kBlockTypeCount ? kBlockIds[t] : "?";
}

BlockType BlockFromId(const std::string& id) {
    for (int t = 1; t < kBlockTypeCount; t++)
        if (id == kBlockIds[t]) return static_cast<BlockType>(t);
    return BlockType::COUNT;
}

int GetCarvedTexture(BlockType type) {
    const int t = static_cast<int>(type);
    return t > 0 && t < kBlockTypeCount ? gCarvedTextureIndices[t].load(std::memory_order_relaxed) : -1;
}

TextureData GetTextureData(BlockType type) {
    int t = static_cast<int>(type);
    if (t < 0 || t >= kBlockTypeCount) return {};
    const auto& i = gBlockTextureIndices[t];
    return { i[0].load(std::memory_order_relaxed), i[1].load(std::memory_order_relaxed),
             i[2].load(std::memory_order_relaxed), i[3].load(std::memory_order_relaxed),
             i[4].load(std::memory_order_relaxed), i[5].load(std::memory_order_relaxed) };
}

namespace {

// A log's face, given the axis it runs along (kFaceAxis terms): its ends (the faces
// along its axis) show the end grain, the rest bark, turned so the grain runs along
// the log. A carved face (`carved`, an atlas index; -1 if not) shows the wood inside
// instead, its grain also along the log.
void OrientGrain(VertexData* v, int n, Face face, int grain, const TextureData& textures, int carved = -1) {
    const int f = static_cast<int>(face);
    const bool end = kFaceAxis[f] == grain;
    if (carved >= 0) {
        for (int i = 0; i < n; i++) v[i].textureIndex = carved;
        if (end) return; // cut across the grain
    }
    else if (end) {
        const bool positive = kFaceOffsets[f][grain] > 0;
        for (int i = 0; i < n; i++) v[i].textureIndex = positive ? textures.top : textures.bottom;
        return;
    }
    else {
        for (int i = 0; i < n; i++) v[i].textureIndex = textures.front;
    }
    // The axis the texture's v (its grain, on bark) runs along on this face.
    int vAxis = -1;
    for (int i = 0; i < n && vAxis < 0; i++)
        for (int j = 0; j < n && vAxis < 0; j++) {
            if (std::abs(v[i].tex.x - v[j].tex.x) > 1e-4f || std::abs(v[i].tex.y - v[j].tex.y) < 1e-4f) continue;
            const glm::vec3 d = glm::abs(v[j].pos - v[i].pos);
            vAxis = d.x >= d.y && d.x >= d.z ? 0 : d.y >= d.z ? 1 : 2;
        }
    if (vAxis >= 0 && vAxis != grain)
        for (int i = 0; i < n; i++) std::swap(v[i].tex.x, v[i].tex.y);
}

} // namespace

void Block::AddSteppedFaces(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading, bool outside) const {
    const TextureData textures = GetTextureData(type);
    const int f = static_cast<int>(face);
    const int texture = face == Face::Top ? textures.top : face == Face::Bottom ? textures.bottom : face == Face::Right ? textures.right
                      : face == Face::Left ? textures.left : face == Face::Back ? textures.back : textures.front;
    const glm::vec3 normal(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]);
    const glm::vec3 origin(x, y, z);
    // Quarter heights in eighths, [qx][qz]: EdgeData's edge 2 is (0,0), 1 is (1,0), 0 is
    // (0,1) and 3 is (1,1).
    const int h[2][2] = { { edgeData.GetTopY(2), edgeData.GetTopY(0) }, { edgeData.GetTopY(1), edgeData.GetTopY(3) } };
    const auto& tangents = kFaceTangents[kFaceAxis[f]];

    // One rectangle, given its corners as seen from outside: a bottom left, b bottom
    // right, c top left, d top right (in eighths, block-local).
    // Faces inside the cell (a quarter's top below the block's, the step up to a taller
    // quarter) are carved: some blocks show their inside there.
    const int carvedTexture = GetCarvedTexture(type);
    auto quad = [&](glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, bool carved) {
        const glm::vec3 corners[4] = { a, b, c, d };
        VertexData v[4];
        for (int i = 0; i < 4; i++) {
            const glm::vec3 p = corners[i] / 8.0f;
            // Texture coordinates follow the face, so quarters show their part of it.
            glm::vec2 uv;
            switch (face) {
            case Face::Right: uv = { 1.0f - p.z, 1.0f - p.y }; break;
            case Face::Left: uv = { p.z, 1.0f - p.y }; break;
            case Face::Back: uv = { p.x, 1.0f - p.y }; break;
            case Face::Front: uv = { 1.0f - p.x, 1.0f - p.y }; break;
            default: uv = { p.x, p.z }; break;
            }
            v[i] = VertexData(origin + p, uv, texture, normal);
            // Shade from the face's corner the vertex is nearest (quarters share the
            // whole face's light and ambient occlusion).
            auto high = [&](int axis) { return axis == 1 ? p.y > 0.5f : p[axis] > 0.5f; };
            const int ia = high(tangents[0]), ib = high(tangents[1]);
            v[i].ao = shading.ao[ia][ib];
            v[i].light = shading.light[ia][ib];
            v[i].sky = shading.sky[ia][ib];
        }
        const int inside = carved ? carvedTexture : -1;
        if (axis != kUpright || inside >= 0) OrientGrain(v, 4, face, GrainAxis(), textures, inside);
        // (The engine's front faces wind clockwise as seen from outside.)
        for (const VertexData& p : { v[0], v[2], v[1], v[1], v[2], v[3] }) {
            vertices.push_back(p.PackPosition());
            vertices.push_back(p.PackShading());
        }
    };

    for (int qx = 0; qx < 2; qx++)
        for (int qz = 0; qz < 2; qz++) {
            const int top = h[qx][qz];
            if (top == 0) continue;
            const float x0 = qx * 4.0f, x1 = x0 + 4.0f, z0 = qz * 4.0f, z1 = z0 + 4.0f, t = static_cast<float>(top);
            switch (face) {
            case Face::Top: quad({ x0, t, z1 }, { x1, t, z1 }, { x0, t, z0 }, { x1, t, z0 }, top < 8); break;
            case Face::Bottom: if (outside) quad({ x0, 0, z0 }, { x1, 0, z0 }, { x0, 0, z1 }, { x1, 0, z1 }, false); break;
            default: {
                // The side of this quarter facing that way: on the block's outside, all
                // of it; inside, only what rises above the quarter next to it.
                const int dx = kFaceOffsets[f][0], dz = kFaceOffsets[f][2];
                const int nx = qx + dx, nz = qz + dz;
                const bool atEdge = nx < 0 || nx > 1 || nz < 0 || nz > 1;
                if (atEdge && !outside) break;
                const float from = atEdge ? 0.0f : static_cast<float>(h[nx][nz]);
                if (from >= t) break;
                switch (face) {
                // Sides on the block's outside are its own (bark); inside, carved.
                case Face::Right: quad({ x1, from, z1 }, { x1, from, z0 }, { x1, t, z1 }, { x1, t, z0 }, !atEdge); break;
                case Face::Left: quad({ x0, from, z0 }, { x0, from, z1 }, { x0, t, z0 }, { x0, t, z1 }, !atEdge); break;
                case Face::Back: quad({ x0, from, z1 }, { x1, from, z1 }, { x0, t, z1 }, { x1, t, z1 }, !atEdge); break;
                default: quad({ x1, from, z0 }, { x0, from, z0 }, { x1, t, z0 }, { x0, t, z0 }, !atEdge); break; // Front
                }
            }
            }
        }
}

void Block::AddFaceVertices(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading) const {
    auto v = GetFaceVertices(face, edgeData, x, y, z, GetTextureData(type));
    if (axis != kUpright) OrientGrain(v.data(), static_cast<int>(v.size()), face, GrainAxis(), GetTextureData(type));

    // Work out which face corner each vertex sits on. Horizontal coordinates are
    // always 0 or 1 within the block; vertical ones may be partial heights, so any
    // vertex above the block's base counts as a top corner.
    const glm::vec3 origin(x, y, z);
    const auto& tangents = kFaceTangents[kFaceAxis[static_cast<int>(face)]];
    auto isHigh = [&](const glm::vec3& pos, int axis) {
        float rel = pos[axis] - origin[axis];
        return axis == 1 ? rel > 0.001f : rel > 0.5f;
    };
    for (auto& vertex : v) {
        const int a = isHigh(vertex.pos, tangents[0]);
        const int b = isHigh(vertex.pos, tangents[1]);
        vertex.ao = shading.ao[a][b];
        vertex.light = shading.light[a][b];
        vertex.sky = shading.sky[a][b];
    }

    // The quad is two triangles (p1, p2, p3) and (p3, p2, p4) sharing the p2-p3
    // diagonal. Interpolating AO across that diagonal gives visible creases when
    // the darker corners lie on it, so for flat (full block) faces split along
    // p1-p4 instead when that pair is brighter.
    const VertexData p1 = v[0], p2 = v[1], p3 = v[2], p4 = v[5];
    if (IsFullBlock() && p1.ao + p4.ao > p2.ao + p3.ao) {
        v = { p1, p2, p4, p4, p3, p1 };
    }

    for (const auto& vertex : v) {
        vertices.push_back(vertex.PackPosition());
        vertices.push_back(vertex.PackShading());
    }
}

namespace {
// A partial block's four floor columns for water (see Block::WaterVolumeAt): heights
// in units and weights (out of `total`).
struct WaterColumns {
    int height[4];
    int weight[4];
    int total;
};
WaterColumns ColumnsOf(const Block& b) {
    constexpr int unitsPerEighth = Block::kMaxWaterVolume / 8;
    WaterColumns c;
    if (b.IsStepped()) {
        for (int e = 0; e < 4; e++) c.height[e] = b.edgeData.GetTopY(e) * unitsPerEighth, c.weight[e] = 1;
        c.total = 4;
        return c;
    }
    // Corners (edge 2 is (0,0), 1 is (1,0), 0 is (0,1), 3 is (1,1)); the surface is
    // split along the (0,1)-(1,0) diagonal, so those two corners count twice.
    const int weights[4] = { 2, 2, 1, 1 }; // edges 0, 1, 2, 3
    for (int e = 0; e < 4; e++) c.height[e] = b.edgeData.GetTopY(e) * unitsPerEighth, c.weight[e] = weights[e];
    c.total = 6;
    return c;
}
} // namespace

int Block::WaterVolumeAt(int level) const {
    const int capacity = WaterCapacity();
    if (!IsPartial()) return std::clamp(level, 0, capacity);
    if (level >= kMaxWaterVolume) return capacity;
    const WaterColumns c = ColumnsOf(*this);
    int sum = 0;
    for (int e = 0; e < 4; e++) sum += c.weight[e] * std::clamp(level - c.height[e], 0, kMaxWaterVolume - c.height[e]);
    return std::min(capacity, sum / c.total);
}

int Block::WaterLevelFor(int volume) const {
    if (!IsPartial()) return volume;
    const WaterColumns c = ColumnsOf(*this);
    int lo = kMaxWaterVolume;
    for (int e = 0; e < 4; e++) lo = std::min(lo, c.height[e]);
    if (volume <= 0) return lo; // dry: its lowest point, where water would go first
    if (volume >= WaterCapacity()) return kMaxWaterVolume;
    // The highest level this much water still reaches: in a narrow notch one unit of
    // water spans several levels, and it stands at the top of them.
    int hi = kMaxWaterVolume;
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (WaterVolumeAt(mid) <= volume) lo = mid; else hi = mid - 1;
    }
    return lo;
}

uint8_t Block::WaterFloor() const {
    if (!IsPartial()) return 0;
    constexpr int unitsPerEighth = kMaxWaterVolume / 8;
    if (IsStepped()) {
        // Four quarters: their mean height.
        int sum = 0;
        for (int e = 0; e < 4; e++) sum += edgeData.GetTopY(e);
        return static_cast<uint8_t>((sum * unitsPerEighth + 2) / 4);
    }
    // Mean height of the two top triangles (see SurfaceHeight), in units.
    const int c00 = edgeData.GetTopY(2), c10 = edgeData.GetTopY(1), c01 = edgeData.GetTopY(0), c11 = edgeData.GetTopY(3);
    const int sixths = c00 + c11 + 2 * (c10 + c01);           // 6 * mean, in eighths
    return static_cast<uint8_t>((sixths * unitsPerEighth + 3) / 6);
}

float Block::SurfaceHeight(float fx, float fz) const {
    if (IsPassable()) return 0.0f;
    if (IsFullBlock()) return 1.0f;

    // Corner heights. See GetFaceVertices(Face::Top): edge 2 is (0,0), 1 is (1,0),
    // 0 is (0,1) and 3 is (1,1), and the quad is split along the (0,1)-(1,0) diagonal.
    const float c00 = edgeData.GetTopY(2) / 8.0f;
    const float c10 = edgeData.GetTopY(1) / 8.0f;
    const float c01 = edgeData.GetTopY(0) / 8.0f;
    const float c11 = edgeData.GetTopY(3) / 8.0f;
    if (IsStepped()) return fx < 0.5f ? (fz < 0.5f ? c00 : c01) : (fz < 0.5f ? c10 : c11); // the quarter's height

    if (fx + fz <= 1.0f) return c00 + (c10 - c00) * fx + (c01 - c00) * fz;
    return c11 + (c01 - c11) * (1.0f - fx) + (c10 - c11) * (1.0f - fz);
}

void Block::AddPlantVertices(std::vector<uint32_t>& vertices, int x, int y, int z, uint8_t blockLight, uint8_t skyLight) const {
    if (type == BlockType::PEBBLES) return AddPebbleVertices(vertices, x, y, z, blockLight, skyLight);
    const float bottom = y - PlantDrop();
    const float top = bottom + 1.0f;
    const int texture = GetTextureData(type).top;
    const glm::vec3 up(0.0f, 1.0f, 0.0f); // lit like an upward face: plants catch the sun

    auto vertex = [&](float vx, float vy, float vz, float u, float v) {
        VertexData d(glm::vec3(vx, vy, vz), glm::vec2(u, v), texture, up);
        d.light = blockLight;
        d.sky = skyLight;
        return d;
    };

    // Two quads along the cell's diagonals. v = 0 is the top of the texture.
    const float diagonals[2][4] = { { 0, 0, 1, 1 }, { 1, 0, 0, 1 } }; // x0, z0, x1, z1
    for (const auto& d : diagonals) {
        VertexData bl = vertex(x + d[0], bottom, z + d[1], 0, 1);
        VertexData br = vertex(x + d[2], bottom, z + d[3], 1, 1);
        VertexData tl = vertex(x + d[0], top, z + d[1], 0, 0);
        VertexData tr = vertex(x + d[2], top, z + d[3], 1, 0);
        // Both windings so the sprite shows from either side with back-face culling on.
        const VertexData quad[12] = { bl, tl, br, br, tl, tr,   bl, br, tl, br, tr, tl };
        for (const auto& q : quad) {
            vertices.push_back(q.PackPosition());
            vertices.push_back(q.PackShading());
        }
    }
}

void Block::AddPebbleVertices(std::vector<uint32_t>& vertices, int x, int y, int z, uint8_t blockLight, uint8_t skyLight) const {
    // Four or five little stones, an eighth high, one or two eighths across, placed
    // by the cell's position (so neighbouring cells differ), on the ground the plant
    // drop says.
    const int texture = GetTextureData(type).bottom; // stone
    const float bottom = y - PlantDrop();
    uint32_t h = static_cast<uint32_t>(x) * 73856093u ^ static_cast<uint32_t>(z) * 19349663u ^ static_cast<uint32_t>(y) * 83492791u;
    auto next = [&](uint32_t range) {
        h ^= h << 13;
        h ^= h >> 17;
        h ^= h << 5;
        return h % range;
    };
    auto vertex = [&](const glm::vec3& p, float u, float v, const glm::vec3& normal) {
        VertexData d(p, glm::vec2(u, v), texture, normal);
        d.light = blockLight;
        d.sky = skyLight;
        return d;
    };
    const int count = 4 + static_cast<int>(next(2));
    for (int i = 0; i < count; i++) {
        const int w = 1 + static_cast<int>(next(2)), d = 1 + static_cast<int>(next(2)); // eighths
        const int px = 1 + static_cast<int>(next(static_cast<uint32_t>(7 - w))), pz = 1 + static_cast<int>(next(static_cast<uint32_t>(7 - d)));
        const glm::vec3 lo(x + px / 8.0f, bottom, z + pz / 8.0f), hi(lo.x + w / 8.0f, bottom + 1.0f / 8.0f, lo.z + d / 8.0f);
        const float u0 = px / 8.0f, v0 = pz / 8.0f, u1 = u0 + w / 8.0f, v1 = v0 + d / 8.0f; // their bit of the texture
        auto quad = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& e, const glm::vec3& n) {
            const VertexData q[4] = { vertex(a, u0, v1, n), vertex(b, u1, v1, n), vertex(c, u0, v0, n), vertex(e, u1, v0, n) };
            // Both windings (like plants): small enough not to matter.
            for (const VertexData& p : { q[0], q[2], q[1], q[1], q[2], q[3], q[0], q[1], q[2], q[1], q[3], q[2] }) {
                vertices.push_back(p.PackPosition());
                vertices.push_back(p.PackShading());
            }
        };
        quad({ lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, hi.z }, { hi.x, hi.y, hi.z }, { 0, 1, 0 }); // top
        quad({ lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { 0, 0, -1 });
        quad({ lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { lo.x, hi.y, hi.z }, { hi.x, hi.y, hi.z }, { 0, 0, 1 });
        quad({ lo.x, lo.y, lo.z }, { lo.x, lo.y, hi.z }, { lo.x, hi.y, lo.z }, { lo.x, hi.y, hi.z }, { -1, 0, 0 });
        quad({ hi.x, lo.y, lo.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { 1, 0, 0 });
    }
}

std::vector<glm::vec3> Block::OutlineSegments() const {
    std::vector<glm::vec3> lines;
    auto edge = [&](glm::vec3 a, glm::vec3 b) {
        lines.push_back(a);
        lines.push_back(b);
    };
    auto box = [&](glm::vec3 lo, glm::vec3 hi) {
        const glm::vec3 c[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, lo.y, hi.z }, { lo.x, lo.y, hi.z },
                                 { lo.x, hi.y, lo.z }, { hi.x, hi.y, lo.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
        for (int i = 0; i < 4; i++) {
            edge(c[i], c[(i + 1) % 4]);         // bottom
            edge(c[4 + i], c[4 + (i + 1) % 4]); // top
            edge(c[i], c[4 + i]);               // sides
        }
    };
    if (type == BlockType::PEBBLES) {
        const float drop = PlantDrop();
        box({ 0.1f, -drop, 0.1f }, { 0.9f, 0.15f - drop, 0.9f });
        return lines;
    }
    if (IsPlant()) {
        const float drop = PlantDrop();
        box({ 0.15f, -drop, 0.15f }, { 0.85f, 0.9f - drop, 0.85f });
        return lines;
    }
    if (IsFullBlock() || IsLiquid()) {
        box(glm::vec3(0.0f), glm::vec3(1.0f));
        return lines;
    }
    // Corner heights at (0,0), (1,0), (1,1), (0,1) (EdgeData edges 2, 1, 3, 0).
    const float h[4] = { edgeData.GetTopY(2) / 8.0f, edgeData.GetTopY(1) / 8.0f, edgeData.GetTopY(3) / 8.0f, edgeData.GetTopY(0) / 8.0f };
    if (IsStepped()) {
        // One box when all four quarters match (a slab, a plate), else each quarter.
        if (h[0] == h[1] && h[1] == h[2] && h[2] == h[3]) {
            box(glm::vec3(0.0f), glm::vec3(1.0f, h[0], 1.0f));
            return lines;
        }
        const glm::vec2 q[4] = { { 0, 0 }, { 0.5f, 0 }, { 0.5f, 0.5f }, { 0, 0.5f } };
        for (int i = 0; i < 4; i++)
            if (h[i] > 0.0f) box({ q[i].x, 0.0f, q[i].y }, { q[i].x + 0.5f, h[i], q[i].y + 0.5f });
        return lines;
    }
    // A slope: its base, the corners up to their heights, the sloped top's edges, and
    // the fold between its two triangles (along (0,1)-(1,0); see GetFaceVertices).
    const glm::vec2 c[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int i = 0; i < 4; i++) {
        const int j = (i + 1) % 4;
        edge({ c[i].x, 0.0f, c[i].y }, { c[j].x, 0.0f, c[j].y });
        edge({ c[i].x, h[i], c[i].y }, { c[j].x, h[j], c[j].y });
        if (h[i] > 0.0f) edge({ c[i].x, 0.0f, c[i].y }, { c[i].x, h[i], c[i].y });
    }
    if (std::abs((h[0] + h[2]) - (h[1] + h[3])) > 0.001f) edge({ 1.0f, h[1], 0.0f }, { 0.0f, h[3], 1.0f });
    return lines;
}

void Block::Shrink() {
    edgeData.Shrink();
    if (!edgeData.IsValid()) {
        type = BlockType::AIR;
        edgeData.MakeFull();
    }
}

int Block::ResourceUnits() const {
    if (IsAir() || IsLiquid()) return 0;
    if (type == BlockType::PEBBLES) return kPebbleUnits;
    if (IsFullBlock() || !CanChisel()) return kUnitsPerBlock;
    if (IsStepped()) {
        // Four quarters: each eighth of a quarter's height is 1/32 of the block.
        int eighths = 0;
        for (int e = 0; e < 4; e++) eighths += edgeData.GetTopY(e);
        return eighths * kUnitsPerBlock / 32;
    }
    // A slope's volume is its mean height (the two triangles it's drawn as).
    const int c00 = edgeData.GetTopY(2), c10 = edgeData.GetTopY(1), c01 = edgeData.GetTopY(0), c11 = edgeData.GetTopY(3);
    const int sixths = c00 + c11 + 2 * (c10 + c01); // 6 * mean height, in eighths
    return (sixths * kUnitsPerBlock + 24) / 48;
}

bool Block::CanChisel() const {
    if (IsAir() || IsLiquid() || IsPlant()) return false;
    switch (type) {
    case BlockType::PUMP:
    case BlockType::PIPE:
    case BlockType::SLUICE:
    case BlockType::PRESSURE_PLATE:
    case BlockType::GLASS:
    case BlockType::CHEST:
    case BlockType::DOOR:
    case BlockType::TNT:
    case BlockType::TNT_LIT:
    case BlockType::BED: return false;
    default: return true;
    }
}

Block Block::Chiselled(int edge, int deltaEighths) const {
    Block b = *this;
    const int height = std::clamp(edgeData.GetTopY(edge) + deltaEighths, 0, 8);
    b.edgeData.SetTopY(edge, static_cast<uint8_t>(height));
    if (b.edgeData.IsFull()) {
        b.water = b.shown = 0; // a full block holds no water
        return b;
    }
    int remaining = 0;
    for (int e = 0; e < 4; e++) remaining += b.edgeData.GetTopY(e);
    if (remaining == 0) return HasWater() ? Water(water) : Block(); // nothing left: its water stays
    b.SetEdgeData(b.edgeData);
    return b;
}

void Block::SetEdgeData(EdgeData edges) {
    // A stepped block stands as long as any quarter does; a slope needs at least two
    // corners above its base.
    bool valid = edges.IsValid();
    if (IsStepped())
        for (int e = 0; e < 4; e++) valid |= edges.GetTopY(e) > edges.GetBottomY(e);
    if (valid) {
        edgeData = edges;
    }
    else {
        type = BlockType::AIR;
        edgeData.MakeFull();
    }
}
