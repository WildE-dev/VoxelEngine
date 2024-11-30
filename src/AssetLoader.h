#include <glad/glad.h>
#include <array>

namespace AssetLoader
{
	GLuint loadCubemap(std::array<const unsigned char*, 6> faces, std::array<const unsigned int, 6> sizes);
	GLuint loadTexture(const unsigned char* bytes, int size);
};