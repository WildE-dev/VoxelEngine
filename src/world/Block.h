#pragma once

#include <cstdint>
#include <functional>
#include <string>
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
    LAMP,
    LOG,
    LEAVES,
    // Plants: drawn as crossed sprites, no collision, don't block light.
    TALL_GRASS,
    FLOWER_RED,
    FLOWER_YELLOW,
    COUNT
};

inline bool IsPlantType(BlockType type) {
    return type == BlockType::TALL_GRASS || type == BlockType::FLOWER_RED || type == BlockType::FLOWER_YELLOW;
}

constexpr uint8_t kMaxLight = 15;

// Light from emitting blocks, and daylight from the sky. Stored and propagated
// separately so day/night can scale sky light in the shader without re-meshing.
enum class LightChannel : uint8_t { Block, Sky };

// Block light emitted by a block type, 0..kMaxLight.
inline uint8_t GetLightEmission(BlockType type) {
    return type == BlockType::LAMP ? kMaxLight : 0;
}

// Indices into the texture atlas for each face of a block.
struct TextureData {
    int top, bottom, front, back, left, right;

    constexpr TextureData() : top(0), bottom(0), front(0), back(0), left(0), right(0) {}
    constexpr explicit TextureData(int all) : top(all), bottom(all), front(all), back(all), left(all), right(all) {}
    constexpr TextureData(int top, int bottom, int front, int back, int left, int right)
        : top(top), bottom(bottom), front(front), back(back), left(left), right(right) {}
};

// Atlas tile indices for each face of a block type. Thread-safe.
TextureData GetTextureData(BlockType type);

// Maps the texture names each block type uses (see Block.cpp) to atlas indices.
// Call after (re)building the atlas, then rebuild chunk meshes. Safe to call while
// the world worker is meshing.
void ResolveBlockTextures(const std::function<int(const std::string&)>& lookup);

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

// Axis (0 = x, 1 = y, 2 = z) each face points along, and the two tangent axes
// spanning it. Used to address the four corners of a face for ambient occlusion.
constexpr int kFaceAxis[kFaceCount] = { 0, 0, 1, 1, 2, 2 };
constexpr int kFaceTangents[3][2] = { {1, 2}, {0, 2}, {0, 1} };

// Per-corner shading for one face, indexed [high side of tangent A][high side of tangent B].
struct FaceShading {
    uint8_t ao[2][2] = { {3, 3}, {3, 3} };    // 0 (darkest) to 3 (unoccluded)
    uint8_t light[2][2] = { {0, 0}, {0, 0} }; // block light, 0..kMaxLight
    uint8_t sky[2][2] = { {0, 0}, {0, 0} };   // sky light, 0..kMaxLight
};

struct Block {
    BlockType type = BlockType::AIR;
    EdgeData edgeData;

    Block() = default;
    explicit Block(BlockType t) : type(t) {}
    // Partial block. Falls back to air if the edge data is degenerate.
    Block(BlockType t, EdgeData edges) : type(t) { SetEdgeData(edges); }

    // A plant whose sprite is drawn `dropEighths` (0..8) below its cell's floor, so it
    // stands on a sloped block underneath. Stored in the otherwise unused edge data.
    static Block Plant(BlockType t, uint8_t dropEighths) {
        Block b(t);
        b.edgeData.edges[0] = dropEighths;
        return b;
    }
    bool IsPlant() const { return IsPlantType(type); }
    float PlantDrop() const { return (edgeData.edges[0] & 0x0F) / 8.0f; }

    void SetEdgeData(EdgeData edges);
    void Shrink();

    bool IsAir() const { return type == BlockType::AIR; }
    // Full, opaque cube that hides any neighbouring face touching it, and collides.
    bool IsFullBlock() const { return type != BlockType::AIR && !IsPlant() && edgeData.IsFull(); }
    // Light passes through air, partial blocks, plants and leaves.
    bool BlocksLight() const { return IsFullBlock() && type != BlockType::LEAVES; }
    // Air or a plant: nothing to stand on or bump into.
    bool IsPassable() const { return IsAir() || IsPlant(); }
    uint8_t GetLightEmission() const { return ::GetLightEmission(type); }

    // Height of the block's top surface (0..1) at a point inside its footprint,
    // fx, fz in [0, 1]. Matches the two triangles the mesher draws for the top face,
    // so things standing on it sit exactly on the visible surface.
    float SurfaceHeight(float fx, float fz) const;

    // Appends a plant's crossed sprite (two double-sided diagonal quads), lit by the
    // light in its own cell.
    void AddPlantVertices(std::vector<uint32_t>& vertices, int x, int y, int z, uint8_t blockLight, uint8_t skyLight) const;

    // Appends 6 vertices (2 packed uint32 each) for the given face.
    void AddFaceVertices(std::vector<uint32_t>& vertices, Face face, int x, int y, int z, const FaceShading& shading) const;

    bool operator==(const Block& o) const { return type == o.type && edgeData == o.edgeData; }
    bool operator!=(const Block& o) const { return !(*this == o); }
};
