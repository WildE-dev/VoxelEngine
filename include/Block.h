#pragma once
#include <vector>
#include <iostream>

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

struct VertexData {
    float x, y, z;
    float u, v;
    float normX, normY, normZ;

    VertexData() : x(0), y(0), z(0), u(0), v(0), normX(0), normY(0), normZ(0) {}
    VertexData(float x, float y, float z, float u, float v, float normX, float normY, float normZ) : x(x), y(y), z(z), u(u), v(v), normX(normX), normY(normY), normZ(normZ) {}

    uint32_t GetVertexInt() {
        int x = this->x * 8;
        int y = this->y * 8;
        int z = this->z * 8;
        int u = this->u;
        int v = this->v;
        int normX = (this->normX + 1.0f) * 1.5f;
        int normY = (this->normY + 1.0f) * 1.5f;
        int normZ = (this->normZ + 1.0f) * 1.5f;
        uint32_t result = 0;
        result |= (x & 0xFF) << 24;
        result |= (y & 0xFF) << 16;
        result |= (z & 0xFF) << 8;
        result |= (u & 0x1) << 7;
        result |= (v & 0x1) << 6;
        result |= (normX & 0x3) << 4;
        result |= (normY & 0x3) << 2;
        result |= (normZ & 0x3);
        //std::cout << x << ", " << y << ", " << z << ", " << result << std::endl;
        
        return result;
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

    void AddFaceVertices(std::vector<uint32_t>& vertices, int face, int x, int y, int z) const;
    void GenerateFaceIndices(std::vector<unsigned int>& indices, unsigned int indexOffset) const;
};

