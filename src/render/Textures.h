#pragma once

#include <array>

#include <glad/glad.h>

namespace Textures
{
    // Faces in GL order: +X, -X, +Y, -Y, +Z, -Z. Returns 0 if any face fails to load.
    GLuint LoadCubemap(const std::array<const char*, 6>& paths);
}
