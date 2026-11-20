#include "world/Block.h"

#include <array>

namespace {

// Unpacked vertex, packed into two uint32s for upload. See main.vert for the decode side.
struct VertexData {
    glm::vec3 pos{0.0f};
    glm::vec2 tex{0.0f};
    int textureIndex = 0;
    glm::vec3 norm{0.0f};

    VertexData() = default;
    VertexData(glm::vec3 pos, glm::vec2 tex, int textureIndex, glm::vec3 norm)
        : pos(pos), tex(tex), textureIndex(textureIndex), norm(norm) {}

    // xyz in 1/8 block units (8 bits each), uv in 1/8 units (4 bits each).
    uint32_t PackPosition() const {
        uint32_t x = static_cast<uint32_t>(pos.x * 8);
        uint32_t y = static_cast<uint32_t>(pos.y * 8);
        uint32_t z = static_cast<uint32_t>(pos.z * 8);
        uint32_t u = static_cast<uint32_t>(tex.x * 8);
        uint32_t v = static_cast<uint32_t>(tex.y * 8);
        return ((x & 0xFFu) << 24) | ((y & 0xFFu) << 16) | ((z & 0xFFu) << 8) | ((u & 0xFu) << 4) | (v & 0xFu);
    }

    // Normal quantised to 4 bits per axis, plus a 20-bit texture index.
    uint32_t PackNormalAndTexture() const {
        uint32_t nx = static_cast<uint32_t>((norm.x + 1.0f) * 7.5f);
        uint32_t ny = static_cast<uint32_t>((norm.y + 1.0f) * 7.5f);
        uint32_t nz = static_cast<uint32_t>((norm.z + 1.0f) * 7.5f);
        return ((nx & 0xFu) << 28) | ((ny & 0xFu) << 24) | ((nz & 0xFu) << 20) | (static_cast<uint32_t>(textureIndex) & 0xFFFFFu);
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

const TextureData& GetTextureData(BlockType type) {
    static constexpr TextureData kNone{};
    static constexpr TextureData kGrass{0, 2, 1, 1, 1, 1};
    static constexpr TextureData kDirt{2};
    static constexpr TextureData kStone{3};

    switch (type) {
    case BlockType::GRASS: return kGrass;
    case BlockType::DIRT:  return kDirt;
    case BlockType::STONE: return kStone;
    default:               return kNone;
    }
}

void Block::AddFaceVertices(std::vector<uint32_t>& vertices, Face face, int x, int y, int z) const {
    auto faceVertices = GetFaceVertices(face, edgeData, x, y, z, GetTextureData(type));
    for (const auto& v : faceVertices) {
        vertices.push_back(v.PackPosition());
        vertices.push_back(v.PackNormalAndTexture());
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
