#pragma once
#include <vector>
#include <iostream>
#include <unordered_map>
#include <tuple>
#include <array>
#include <glm/glm.hpp>

struct EdgeData {
    uint8_t edges[4];

    EdgeData() {
        MakeFull();
    }

    void MakeFull() {
        for (int i = 0; i < 4; ++i) {
            edges[i] = 0x80;
        }
    }

    bool IsValid()
    {
        int sameCount = 0;
        for (size_t i = 0; i < 4; i++)
        {
            uint8_t top = GetTopY(i);
            uint8_t bottom = GetBottomY(i);

            if (top == bottom)
                sameCount++;
        }

        if (sameCount == 4)
            return false;

        return true;
    }

    void Shrink() {
        for (size_t i = 0; i < 4; i++)
        {
            SetTopY(i, GetTopY(i) - 1);
        }
    }

    void SetBottomY(int edgeIndex, uint8_t value) {
        if (edgeIndex < 0 || edgeIndex >= 4 || value > 8) return;
        edges[edgeIndex] = (edges[edgeIndex] & 0xF0) | (value & 0x0F);
    }

    void SetTopY(int edgeIndex, uint8_t value) {
        if (edgeIndex < 0 || edgeIndex >= 4 || value > 8) return;
        edges[edgeIndex] = (edges[edgeIndex] & 0x0F) | ((value & 0x0F) << 4);
    }

    uint8_t GetBottomY(int edgeIndex) const {
        if (edgeIndex < 0 || edgeIndex >= 4) return 0;
        return edges[edgeIndex] & 0x0F;
    }

    uint8_t GetTopY(int edgeIndex) const {
        if (edgeIndex < 0 || edgeIndex >= 4) return 0;
        return (edges[edgeIndex] >> 4) & 0x0F;
    }
};

struct VertexData {
    glm::vec3 pos;
    glm::vec2 tex;
    int textureIndex;
    glm::vec3 norm;

    VertexData() : pos(glm::vec3(0, 0, 0)), tex(glm::vec2(0, 0)), textureIndex(0), norm(glm::vec3(0, 0, 0)) {}
    VertexData(glm::vec3 pos, glm::vec2 tex, int textureIndex, glm::vec3 norm) : pos(pos), tex(tex), textureIndex(textureIndex), norm(norm) {}

    uint32_t GetVertexInt() {
        uint32_t x = this->pos.x * 8;
        uint32_t y = this->pos.y * 8;
        uint32_t z = this->pos.z * 8;
        uint32_t u = this->tex.x * 8;
        uint32_t v = this->tex.y * 8;
        uint32_t result = 0;
        result |= (x & 0xFFu) << 24;
        result |= (y & 0xFFu) << 16;
        result |= (z & 0xFFu) << 8;
        result |= (u & 0xFu) << 4;
        result |= v & 0xFu;
        
        return result;
    }

    uint32_t GetVertexNormInt() {
        uint32_t normX = (this->norm.x + 1.0f) * 7.5f;
        uint32_t normY = (this->norm.y + 1.0f) * 7.5f;
        uint32_t normZ = (this->norm.z + 1.0f) * 7.5f;
        uint32_t result = 0;
        result |= (normX & 0xFu) << 28;
        result |= (normY & 0xFu) << 24;
        result |= (normZ & 0xFu) << 20;
        result |= textureIndex & 0xFFFFFu;

        return result;
    }
};

enum class BlockType : uint8_t {
    AIR,
    GRASS,
    DIRT,
    STONE,
};

struct TextureData {
    int top, bottom, front, back, left, right;

    TextureData() : top(0), bottom(0), front(0), back(0), left(0), right(0) {}
    TextureData(int all) : top(all), bottom(all), front(all), back(all), left(all), right(all) {}
    TextureData(int top, int bottom, int front, int back, int left, int right) : top(top), bottom(bottom), front(front), back(back), left(left), right(right) {}
};

static std::unordered_map<BlockType, TextureData> blockTextureMap = {
    {BlockType::GRASS, TextureData(0, 2, 1, 1, 1, 1)},
    {BlockType::DIRT, TextureData(2)},
    {BlockType::STONE, TextureData(3)},
};

class Block
{
public:
    BlockType type;
    EdgeData edgeData;

    Block();
    Block(BlockType t);

    // Copy constructor
    Block(const Block& other);

    // Assignment operator
    Block& operator=(const Block& other);

    void AddFaceVertices(std::vector<uint32_t>& vertices, int face, int x, int y, int z) const;
    void Shrink();
    void SetEdgeData(EdgeData edgeData);

    bool IsFullBlock() const;
};

