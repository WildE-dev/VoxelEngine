#pragma once
#include <glad/glad.h>
#include <array>

namespace AssetLoader
{
	GLuint loadCubemap(std::array<const char*, 6> paths);
	GLuint loadTexture(const char* path);
};