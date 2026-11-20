#include "world/Block.h"

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
};

// Resolved atlas indices. Written on the main thread on (re)load, read by the
// mesher on the worker thread, hence atomic.
std::atomic<int> gBlockTextureIndices[kBlockTypeCount][6] = {};

} // namespace

void ResolveBlockTextures(const std::function<int(const std::string&)>& lookup) {
    for (int type = 0; type < kBlockTypeCount; type++) {
        for (int face = 0; face < 6; face++) {
            const char* name = kBlockTextureNames[type].faces[face];
            gBlockTextureIndices[type][face] = name ? lookup(name) : 0;
        }
    }
}

TextureData GetTextureData(BlockType type) {
    int t = static_cast<int>(type);
    if (t < 0 || t >= kBlockTypeCount) return {};
    const auto& i = gBlockTextureIndices[t];
    return { i[0].load(std::memory_order_relaxed), i[1].load(std::memory_order_relaxed),
             i[2].load(std::memory_order_relaxed), i[3].load(std::memory_order_relaxed),
             i[4].load(std::memory_order_relaxed), i[5].load(std::memory_order_relaxed) };
}

void Block::AddFaceVertices(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading) const {
    auto v = GetFaceVertices(face, edgeData, x, y, z, GetTextureData(type));

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

float Block::SurfaceHeight(float fx, float fz) const {
    if (IsPassable()) return 0.0f;
    if (IsFullBlock()) return 1.0f;

    // Corner heights. See GetFaceVertices(Face::Top): edge 2 is (0,0), 1 is (1,0),
    // 0 is (0,1) and 3 is (1,1), and the quad is split along the (0,1)-(1,0) diagonal.
    const float c00 = edgeData.GetTopY(2) / 8.0f;
    const float c10 = edgeData.GetTopY(1) / 8.0f;
    const float c01 = edgeData.GetTopY(0) / 8.0f;
    const float c11 = edgeData.GetTopY(3) / 8.0f;

    if (fx + fz <= 1.0f) return c00 + (c10 - c00) * fx + (c01 - c00) * fz;
    return c11 + (c01 - c11) * (1.0f - fx) + (c10 - c11) * (1.0f - fz);
}

void Block::AddPlantVertices(std::vector<uint32_t>& vertices, int x, int y, int z, uint8_t blockLight, uint8_t skyLight) const {
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

void Block::Shrink() {
    edgeData.Shrink();
    if (!edgeData.IsValid()) {
        type = BlockType::AIR;
        edgeData.MakeFull();
    }
}

void Block::SetEdgeData(EdgeData edges) {
    if (edges.IsValid()) {
        edgeData = edges;
    }
    else {
        type = BlockType::AIR;
        edgeData.MakeFull();
    }
}
