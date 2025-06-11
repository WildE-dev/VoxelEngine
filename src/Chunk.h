#pragma once
#include <array>
#include <glad/glad.h>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>

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
	
	bool IsLoaded() const;
	bool IsSetup() const;
	bool NeedsRebuilding() const;
	
	bool IsEmpty() const;
	bool IsFull() const;
	bool IsSurrounded() const;

	void UpdateEmptyFullFlags();
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

	void LoadChunk(TerrainGenerator* terrainGenerator);
	void SetupChunk();
	void UnloadChunk();
	void GenerateMesh();
	void SendVertexData();
	void Render(Shader& shader);

private:
	std::array<Block, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE> blocks;
	std::vector<uint32_t> vertices;

	std::mutex block_mutex;

	World* world;

	GLuint VAO, VBO;
	int chunkX, chunkY, chunkZ;
	std::atomic_bool isLoaded;
	std::atomic_bool isMeshSent;
	std::atomic_bool isInitialized;
	bool isSetup;
	bool needsRebuilding;

	bool isEmpty;
	bool isFull;
	bool isSurrounded;

	void Initialize();
	void Clear();

	int Index(int x, int y, int z) const;
};

