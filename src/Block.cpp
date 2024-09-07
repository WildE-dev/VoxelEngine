#include "Block.h"

Block::Block() : type(BlockType::AIR), edgeData() {}
Block::Block(BlockType t) : type(t), edgeData() {}

Block::Block(const Block& other) : type(other.type), edgeData(other.edgeData) {}

Block& Block::operator=(const Block& other) {
    if (this != &other) {
        type = other.type;
        edgeData = other.edgeData;
        // Copy other properties as needed
    }
    return *this;
}

enum class Face {
    Right = 0,
    Left = 1,
    Top = 2,
    Bottom = 3,
    Back = 4,
    Front = 5
};

glm::vec3 GetNormal(glm::vec3 p1, glm::vec3 p2, glm::vec3 p3) {
    glm::vec3 A = p3 - p1;
    glm::vec3 B = p2 - p1;
    glm::vec3 cross = glm::normalize(glm::cross(A, B));
    return cross;
}

std::array<VertexData, 6> GetFaceVertices(Face face, const EdgeData& edgeData, int x, int y, int z, TextureData textureData) {
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

void Block::AddFaceVertices(std::vector<uint32_t>& vertices, int face, int x, int y, int z) const {
    auto faceVertices = GetFaceVertices(static_cast<Face>(face), edgeData, x, y, z, blockTextureMap[type]);

    for (size_t i = 0; i < 6; i++)
    {
        vertices.push_back(faceVertices[i].GetVertexInt());
        vertices.push_back(faceVertices[i].GetVertexNormInt());
    }
}

void Block::Shrink() {
    edgeData.Shrink();
    if (!edgeData.IsValid()) {
        type = BlockType::AIR;
    }
}

void Block::SetEdgeData(EdgeData edgeData)
{
    if (!edgeData.IsValid()) {
        type = BlockType::AIR;
    }
    else {
        this->edgeData = edgeData;
    }
}

bool Block::IsFullBlock() const
{
    for (size_t i = 0; i < 4; i++)
    {
        if (edgeData.edges[i] != 0x80)
            return false;
    }

    return true;
}