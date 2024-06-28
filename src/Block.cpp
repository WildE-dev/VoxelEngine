#include <tuple>
#include <array>

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


std::array<VertexData, 4> GetFaceVertices(Face face, const EdgeData& edgeData, int x, int y, int z, TextureData textureData) {
    std::array<VertexData, 4> vertices;

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
        vertices[0] = VertexData(x + 1.0f, y + bottomY1, z,         1.0f, 1.0f, textureData.right, 1.0f, 0.0f, 0.0f);
        vertices[1] = VertexData(x + 1.0f, y + bottomY3, z + 1.0f,  0.0f, 1.0f, textureData.right, 1.0f, 0.0f, 0.0f);
        vertices[2] = VertexData(x + 1.0f, y + topY1, z,            1.0f, 0.0f, textureData.right, 1.0f, 0.0f, 0.0f);
        vertices[3] = VertexData(x + 1.0f, y + topY3, z + 1.0f,     0.0f, 0.0f, textureData.right, 1.0f, 0.0f, 0.0f);
        break;
    case Face::Left:
        vertices[0] = VertexData(x, y + bottomY0, z + 1.0f,         1.0f, 1.0f, textureData.left, -1.0f, 0.0f, 0.0f);
        vertices[1] = VertexData(x, y + bottomY2, z,                0.0f, 1.0f, textureData.left, -1.0f, 0.0f, 0.0f);
        vertices[2] = VertexData(x, y + topY0, z + 1.0f,            1.0f, 0.0f, textureData.left, -1.0f, 0.0f, 0.0f);
        vertices[3] = VertexData(x, y + topY2, z,                   0.0f, 0.0f, textureData.left, -1.0f, 0.0f, 0.0f);
        break;
    case Face::Top:
        vertices[0] = VertexData(x + 1.0f, y + topY3, z + 1.0f,     1.0f, 1.0f, textureData.top, 0.0f, 1.0f, 0.0f);
        vertices[1] = VertexData(x, y + topY0, z + 1.0f,            0.0f, 1.0f, textureData.top, 0.0f, 1.0f, 0.0f);
        vertices[2] = VertexData(x + 1.0f, y + topY1, z,            1.0f, 0.0f, textureData.top, 0.0f, 1.0f, 0.0f);
        vertices[3] = VertexData(x, y + topY2, z,                   0.0f, 0.0f, textureData.top, 0.0f, 1.0f, 0.0f);
        break;
    case Face::Bottom:
        vertices[0] = VertexData(x, y + bottomY2, z,                1.0f, 1.0f, textureData.bottom, 0.0f, -1.0f, 0.0f);
        vertices[1] = VertexData(x, y + bottomY0, z + 1.0f,         0.0f, 1.0f, textureData.bottom, 0.0f, -1.0f, 0.0f);
        vertices[2] = VertexData(x + 1.0f, y + bottomY1, z,         1.0f, 0.0f, textureData.bottom, 0.0f, -1.0f, 0.0f);
        vertices[3] = VertexData(x + 1.0f, y + bottomY3, z + 1.0f,  0.0f, 0.0f, textureData.bottom, 0.0f, -1.0f, 0.0f);
        break;
    case Face::Back:
        vertices[0] = VertexData(x, y + bottomY0, z + 1.0f,         0.0f, 1.0f, textureData.back, 0.0f, 0.0f, -1.0f);
        vertices[1] = VertexData(x, y + topY0, z + 1.0f,            0.0f, 0.0f, textureData.back, 0.0f, 0.0f, -1.0f);
        vertices[2] = VertexData(x + 1.0f, y + bottomY3, z + 1.0f,  1.0f, 1.0f, textureData.back, 0.0f, 0.0f, -1.0f);
        vertices[3] = VertexData(x + 1.0f, y + topY3, z + 1.0f,     1.0f, 0.0f, textureData.back, 0.0f, 0.0f, -1.0f);
        break;
    case Face::Front:
        vertices[0] = VertexData(x + 1.0f, y + bottomY1, z,         0.0f, 1.0f, textureData.front, 0.0f, 0.0f, 1.0f);
        vertices[1] = VertexData(x + 1.0f, y + topY1, z,            0.0f, 0.0f, textureData.front, 0.0f, 0.0f, 1.0f);
        vertices[2] = VertexData(x, y + bottomY2, z,                1.0f, 1.0f, textureData.front, 0.0f, 0.0f, 1.0f);
        vertices[3] = VertexData(x, y + topY2, z,                   1.0f, 0.0f, textureData.front, 0.0f, 0.0f, 1.0f);
        break;
    }

    return vertices;
}

void Block::AddFaceVertices(std::vector<uint32_t>& vertices, int face, int x, int y, int z) const {
    auto faceVertices = GetFaceVertices(static_cast<Face>(face), edgeData, x, y, z, blockTextureMap[type]);

    const int order[] = { 0, 1, 2, 2, 1, 3 };

    for (size_t i = 0; i < 6; i++)
    {
        vertices.push_back(faceVertices[order[i]].GetVertexInt());
        vertices.push_back(faceVertices[order[i]].GetVertexNormInt());
    }

    //vertices.push_back(faceVertices[0].GetVertexInt());
    //vertices.push_back(faceVertices[1].GetVertexInt());
    //vertices.push_back(faceVertices[2].GetVertexInt());
    //vertices.push_back(faceVertices[2].GetVertexInt());
    //vertices.push_back(faceVertices[1].GetVertexInt());
    //vertices.push_back(faceVertices[3].GetVertexInt());
}