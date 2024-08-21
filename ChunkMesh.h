#pragma once
#include <glad/gl.h>
#include <vector>

class Chunk;
class Shader;
class World;

class ChunkMesh {
public:
    ChunkMesh();
    ~ChunkMesh();

    // Copy constructor
    ChunkMesh(const ChunkMesh& other);

    // Copy assignment operator
    ChunkMesh& operator=(const ChunkMesh& other);

    // Move constructor
    ChunkMesh(ChunkMesh&& other) noexcept;

    // Move assignment operator
    ChunkMesh& operator=(ChunkMesh&& other) noexcept;

    void GenerateMesh(const Chunk& chunk, World& world, int chunkX, int chunkY, int chunkZ);
    void Render(Shader& shader);

private:
    GLuint VAO, VBO;
    GLsizei vertexCount;

    void Initialize();
    void Clear();
};

