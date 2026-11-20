#include "render/DebugDraw.h"

#include <glm/gtc/matrix_transform.hpp>

#include "render/Shader.h"

namespace {

const float kCubeLines[] = {
    // back face
    0,0,0, 1,0,0,
    1,0,0, 1,1,0,
    1,1,0, 0,1,0,
    0,1,0, 0,0,0,
    // front face
    0,0,1, 1,0,1,
    1,0,1, 1,1,1,
    1,1,1, 0,1,1,
    0,1,1, 0,0,1,
    // connecting edges
    0,0,0, 0,0,1,
    0,1,0, 0,1,1,
    1,0,0, 1,0,1,
    1,1,0, 1,1,1,
};

constexpr GLsizei kCubeLineVertexCount = sizeof(kCubeLines) / (3 * sizeof(float));

} // namespace

DebugDraw::DebugDraw() {
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeLines), kCubeLines, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
}

DebugDraw::~DebugDraw() {
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
}

void DebugDraw::DrawWireCube(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const glm::vec3& position) {
    shader.Use();
    shader.SetUniform("model", glm::translate(glm::mat4(1.0f), position));
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);

    GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, kCubeLineVertexCount);

    if (depthWasEnabled) glEnable(GL_DEPTH_TEST);
}
