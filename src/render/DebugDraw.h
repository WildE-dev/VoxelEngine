#pragma once

#include <vector>

#include <glad/glad.h>
#include <glm/glm.hpp>

class Shader;

// Draws the outline of the block in view (as lines, depth tested: hidden behind
// other blocks).
class DebugDraw
{
public:
    DebugDraw();
    ~DebugDraw();

    DebugDraw(const DebugDraw&) = delete;
    DebugDraw& operator=(const DebugDraw&) = delete;

    // Pairs of block-local points around `position`.
    void DrawOutline(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const glm::vec3& position,
                     const std::vector<glm::vec3>& segments);

private:
    GLuint outlineVao = 0, outlineVbo = 0;
};
