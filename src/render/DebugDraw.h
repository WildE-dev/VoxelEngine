#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

class Shader;

// Draws a unit wireframe cube, used to highlight the block under the crosshair.
class DebugDraw
{
public:
    DebugDraw();
    ~DebugDraw();

    DebugDraw(const DebugDraw&) = delete;
    DebugDraw& operator=(const DebugDraw&) = delete;

    void DrawWireCube(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const glm::vec3& position);

private:
    GLuint vao = 0, vbo = 0;
};
