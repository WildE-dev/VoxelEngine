#include <tuple>
#include <array>

#include "Block.h"

Block::Block() : type(0), edgeData() {}
Block::Block(unsigned char t) : type(t), edgeData() {}

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


std::array<std::tuple<float, float, float, float, float, float, float, float>, 4> GetFaceVertices(Face face, const EdgeData& edgeData, int x, int y, int z) {
    std::array<std::tuple<float, float, float, float, float, float, float, float>, 4> vertices;

    float bottomY0 = static_cast<float>(edgeData.GetBottomY(0)) / 8.0f - 0.5f;
    float bottomY1 = static_cast<float>(edgeData.GetBottomY(1)) / 8.0f - 0.5f;
    float bottomY2 = static_cast<float>(edgeData.GetBottomY(2)) / 8.0f - 0.5f;
    float bottomY3 = static_cast<float>(edgeData.GetBottomY(3)) / 8.0f - 0.5f;

    float topY0 = static_cast<float>(edgeData.GetTopY(0)) / 8.0f - 0.5f;
    float topY1 = static_cast<float>(edgeData.GetTopY(1)) / 8.0f - 0.5f;
    float topY2 = static_cast<float>(edgeData.GetTopY(2)) / 8.0f - 0.5f;
    float topY3 = static_cast<float>(edgeData.GetTopY(3)) / 8.0f - 0.5f;

    switch (face) {
    case Face::Right:
        vertices[0] = std::make_tuple(x + 0.5f, y + bottomY1, z - 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        vertices[1] = std::make_tuple(x + 0.5f, y + bottomY3, z + 0.5f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f);
        vertices[2] = std::make_tuple(x + 0.5f, y + topY1, z - 0.5f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);
        vertices[3] = std::make_tuple(x + 0.5f, y + topY3, z + 0.5f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f);
        break;
    case Face::Left:
        vertices[0] = std::make_tuple(x - 0.5f, y + bottomY0, z + 0.5f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
        vertices[1] = std::make_tuple(x - 0.5f, y + bottomY2, z - 0.5f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f);
        vertices[2] = std::make_tuple(x - 0.5f, y + topY0, z + 0.5f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f);
        vertices[3] = std::make_tuple(x - 0.5f, y + topY2, z - 0.5f, 1.0f, 1.0f, -1.0f, 0.0f, 0.0f);
        break;
    case Face::Top:
        vertices[0] = std::make_tuple(x + 0.5f, y + topY3, z + 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
        vertices[1] = std::make_tuple(x - 0.5f, y + topY0, z + 0.5f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f);
        vertices[2] = std::make_tuple(x + 0.5f, y + topY1, z - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f);
        vertices[3] = std::make_tuple(x - 0.5f, y + topY2, z - 0.5f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f);
        break;
    case Face::Bottom:
        vertices[0] = std::make_tuple(x - 0.5f, y + bottomY2, z - 0.5f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f);
        vertices[1] = std::make_tuple(x - 0.5f, y + bottomY0, z + 0.5f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f);
        vertices[2] = std::make_tuple(x + 0.5f, y + bottomY1, z - 0.5f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f);
        vertices[3] = std::make_tuple(x + 0.5f, y + bottomY3, z + 0.5f, 1.0f, 1.0f, 0.0f, -1.0f, 0.0f);
        break;
    case Face::Back:
        vertices[0] = std::make_tuple(x - 0.5f, y + bottomY0, z + 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f);
        vertices[1] = std::make_tuple(x - 0.5f, y + topY0, z + 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f);
        vertices[2] = std::make_tuple(x + 0.5f, y + bottomY3, z + 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, -1.0f);
        vertices[3] = std::make_tuple(x + 0.5f, y + topY3, z + 0.5f, 1.0f, 1.0f, 0.0f, 0.0f, -1.0f);
        break;
    case Face::Front:
        vertices[0] = std::make_tuple(x + 0.5f, y + bottomY1, z - 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        vertices[1] = std::make_tuple(x + 0.5f, y + topY1, z - 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        vertices[2] = std::make_tuple(x - 0.5f, y + bottomY2, z - 0.5f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f);
        vertices[3] = std::make_tuple(x - 0.5f, y + topY2, z - 0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f);
        break;
    }

    return vertices;
}

void Block::GenerateFaceIndices(std::vector<unsigned int>& indices, unsigned int indexOffset) const {
    indices.push_back(indexOffset);
    indices.push_back(indexOffset + 1);
    indices.push_back(indexOffset + 2);
    indices.push_back(indexOffset + 2);
    indices.push_back(indexOffset + 1);
    indices.push_back(indexOffset + 3);
}

void Block::AddFaceVertices(std::vector<float>& vertices, int face, int x, int y, int z) const {
    auto faceVertices = GetFaceVertices(static_cast<Face>(face), edgeData, x, y, z);
    unsigned int indexOffset = static_cast<unsigned int>(vertices.size() / 8);

    for (const auto& vertex : faceVertices) {
        vertices.push_back(std::get<0>(vertex));
        vertices.push_back(std::get<1>(vertex));
        vertices.push_back(std::get<2>(vertex));

        // Texture coords (example, you can set proper texture coordinates)
        vertices.push_back(std::get<3>(vertex));
        vertices.push_back(std::get<4>(vertex));

        // Normals
        vertices.push_back(std::get<5>(vertex));
        vertices.push_back(std::get<6>(vertex));
        vertices.push_back(std::get<7>(vertex));
    }

    //for (int i = 0; i < 6; ++i) {
    //    // Determine the y positions using EdgeData
    //    float bottomY = static_cast<float>(edgeData.GetBottomY(i % 4)) / 15.0f - 0.5f;
    //    float topY = static_cast<float>(edgeData.GetTopY(i % 4)) / 15.0f - 0.5f;

    //    float vertexY = (i < 2) ? bottomY : topY;

    //    // Position
    //    vertices.push_back(blockVertices[face][i * 8] + x);
    //    vertices.push_back(vertexY + y);
    //    vertices.push_back(blockVertices[face][i * 8 + 2] + z);

    //    // Texture coords
    //    vertices.push_back(blockVertices[face][i * 8 + 3]);
    //    vertices.push_back(blockVertices[face][i * 8 + 4]);

    //    // Normals
    //    vertices.push_back(blockVertices[face][i * 8 + 5]);
    //    vertices.push_back(blockVertices[face][i * 8 + 6]);
    //    vertices.push_back(blockVertices[face][i * 8 + 7]);
    //}
}