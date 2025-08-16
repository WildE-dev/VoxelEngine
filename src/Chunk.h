#pragma once
#include <array>
#include <glad/glad.h>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>

#include "TerrainGenerator.h"
#include "Block.h"

class World;
class Shader;

class Chunk
{
public:
	// 31 is the max because the shader uses 1 byte for position and 32 * 8 = 256
	// which is greater than 255, the max 1 byte can store
	static const int CHUNK_SIZE = 16; 

	static int chunkCount;

	//Chunk();
	Chunk(World* world, int chunkX, int chunkY, int chunkZ);
	~Chunk();
	Chunk(const Chunk& other);
	Chunk& operator=(const Chunk& other);
	Chunk(Chunk&& other) noexcept;
	Chunk& operator=(Chunk&& other) noexcept;
	
	void Reset(int chunkX, int chunkY, int chunkZ);

	bool IsLoaded() const;
	bool IsSetup() const;
	bool NeedsRebuilding() const;
	
	bool IsEmpty() const;
	bool IsFull() const;
	bool IsSurrounded() const;

	void UpdateEmptyFullFlags();
	void UpdateChunkSurroundedFlag();
	void SetIsSurrounded(bool value);
	void SetNeedsRebuilding(bool value);

	bool ShouldRender();

	glm::ivec3 GetCoords();

	const Block& GetBlock(int x, int y, int z);
	bool GetBlockCulls(int x, int y, int z);
	void SetBlock(int x, int y, int z, Block block);
	void SetBlock(int x, int y, int z, BlockType type);
	void SetBlock(int x, int y, int z, EdgeData edges);
	void SetBlock(int x, int y, int z, BlockType type, EdgeData edges);

	bool IsGeneratingMesh() const { return isGeneratingMesh; }
	void InvalidateNeighborCache() { neighborCacheValid = false; }

	void LoadChunk(TerrainGenerator* terrainGenerator);
	void SetupChunk();
	void UnloadChunk();
	void GenerateMesh();
	void SendVertexData();
	void Render(Shader& shader);

	void DebugPrintState() const {
        std::cout << "Chunk (" << chunkX << "," << chunkY << "," << chunkZ << "): "
                  << "Loaded=" << isLoaded 
                  << " Setup=" << isSetup 
                  << " Empty=" << isEmpty 
                  << " Full=" << isFull 
                  << " Surrounded=" << isSurrounded 
                  << " NeedsRebuild=" << needsRebuilding 
                  << " VertexCount=" << vertex_count 
                  << " MeshSent=" << isMeshSent << std::endl;
    }

private:
	std::array<Block, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE> blocks;
	std::vector<uint32_t> vertices;
	std::atomic<size_t> vertex_count{ 0 };  // Track renderable vertex count

	std::atomic<bool> isGeneratingMesh{ false };
	bool hasVisibleFaces = true; // Cache whether chunk has any visible faces

	// Cache neighbor chunks to avoid repeated lookups
	mutable Chunk* cachedNeighbors[6] = { nullptr };
	mutable bool neighborCacheValid = false;

	std::mutex block_mutex;

	World* world;

	GLuint VAO, VBO;
	int chunkX, chunkY, chunkZ;
	std::atomic<bool> isLoaded;
	std::atomic<bool> isMeshSent;
	std::atomic<bool> isInitialized;
	std::atomic<bool> isSetup;
	std::atomic<bool> needsRebuilding;

	bool isEmpty;
	bool isFull;
	bool isSurrounded;

	void InitializeMeshBuffers();
	void Clear();

	int Index(int x, int y, int z) const;
};

