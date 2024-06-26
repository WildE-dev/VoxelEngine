#pragma once
#include <vector>

struct EdgeData {
    uint8_t edges[4];

    EdgeData() {
        for (int i = 0; i < 4; ++i) {
            edges[i] = 0x80;// +i - (i << 4);
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

class Block
{
public:
    unsigned char type;
    EdgeData edgeData;

    Block();
    Block(unsigned char t);

    // Copy constructor
    Block(const Block& other);

    // Assignment operator
    Block& operator=(const Block& other);

    void AddFaceVertices(std::vector<float>& vertices, int face, int x, int y, int z) const;
    void GenerateFaceIndices(std::vector<unsigned int>& indices, unsigned int indexOffset) const;
};

