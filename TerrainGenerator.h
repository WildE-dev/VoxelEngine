#pragma once

class TerrainGenerator {
public:
	float GetHeight(int x, int z);
private:
	const int OCTAVES = 8;
	const float SIZE = 120.0f;
	const float AMPLITUDE = 20.0f;
};

