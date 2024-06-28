#pragma once
#include <array>
#include <glad/gl.h>
#include <map>
#include <memory>

#include "Block.h";
#include "ChunkMesh.h"

class Shader;

class Chunk
{
public:
	static const int CHUNK_SIZE = 16;

	Chunk();
	Chunk(World* world, int chunkX, int chunkY, int chunkZ);
	~Chunk();
	Chunk(const Chunk& other);
	Chunk& operator=(const Chunk& other);
	Chunk(Chunk&& other) noexcept;
	Chunk& operator=(Chunk&& other) noexcept;

	void GenerateMesh(World& world);
	void Render(Shader& shader);

	const Block& GetBlock(int x, int y, int z) const;
	bool GetBlockCulls(int x, int y, int z) const;
	void SetBlock(int x, int y, int z, Block block, bool regenerateMesh = true);
	void SetBlock(int x, int y, int z, BlockType type, bool regenerateMesh = true);
	void SetBlock(int x, int y, int z, EdgeData edges, bool regenerateMesh = true);
private:
	std::array<Block, CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE> blocks;
	ChunkMesh mesh;

	World* world;

	int chunkX, chunkY, chunkZ;

	int Index(int x, int y, int z) const;
};

