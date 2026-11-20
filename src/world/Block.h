#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

// Per-column heights for a partial ("sloped") block. Each of the 4 vertical
// edges stores a bottom and top height in eighths of a block (0..8), packed as
// top in the high nibble and bottom in the low nibble.
struct EdgeData {
    uint8_t edges[4];

    EdgeData() { MakeFull(); }

    void MakeFull() {
        for (auto& e : edges) e = 0x80; // top = 8, bottom = 0
    }

    bool IsFull() const {
        for (auto e : edges)
            if (e != 0x80) return false;
        return true;
    }

    // A block is only valid if at most two of its edges are collapsed.
    bool IsValid() const {
        int collapsed = 0;
        for (int i = 0; i < 4; i++)
            if (GetTopY(i) == GetBottomY(i)) collapsed++;
        return collapsed <= 2;
    }

    void Shrink() {
        for (int i = 0; i < 4; i++) {
            uint8_t top = GetTopY(i);
            if (top > 0) SetTopY(i, top - 1);
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

    bool operator==(const EdgeData& o) const {
        for (int i = 0; i < 4; i++)
            if (edges[i] != o.edges[i]) return false;
        return true;
    }
};

enum class BlockType : uint8_t {
    AIR,
    GRASS,
    DIRT,
    STONE,
};

// Indices into the texture atlas for each face of a block.
struct TextureData {
    int top, bottom, front, back, left, right;

    constexpr TextureData() : top(0), bottom(0), front(0), back(0), left(0), right(0) {}
    constexpr explicit TextureData(int all) : top(all), bottom(all), front(all), back(all), left(all), right(all) {}
    constexpr TextureData(int top, int bottom, int front, int back, int left, int right)
        : top(top), bottom(bottom), front(front), back(back), left(left), right(right) {}
};

const TextureData& GetTextureData(BlockType type);

// Face order is shared by the mesher, the vertex generator and the neighbour lookup.
enum class Face : int {
    Right = 0,  // +X
    Left = 1,   // -X
    Top = 2,    // +Y
    Bottom = 3, // -Y
    Back = 4,   // +Z
    Front = 5,  // -Z
};

constexpr int kFaceCount = 6;
constexpr int kFaceOffsets[kFaceCount][3] = {
    { 1,  0,  0},
    {-1,  0,  0},
    { 0,  1,  0},
    { 0, -1,  0},
    { 0,  0,  1},
    { 0,  0, -1},
};

struct Block {
    BlockType type = BlockType::AIR;
    EdgeData edgeData;

    Block() = default;
    explicit Block(BlockType t) : type(t) {}
    // Partial block. Falls back to air if the edge data is degenerate.
    Block(BlockType t, EdgeData edges) : type(t) { SetEdgeData(edges); }

    void SetEdgeData(EdgeData edges);
    void Shrink();

    bool IsAir() const { return type == BlockType::AIR; }
    // Full, opaque cube that hides any neighbouring face touching it.
    bool IsFullBlock() const { return type != BlockType::AIR && edgeData.IsFull(); }

    // Appends 6 vertices (2 packed uint32 each) for the given face.
    void AddFaceVertices(std::vector<uint32_t>& vertices, Face face, int x, int y, int z) const;

    bool operator==(const Block& o) const { return type == o.type && edgeData == o.edgeData; }
    bool operator!=(const Block& o) const { return !(*this == o); }
};
