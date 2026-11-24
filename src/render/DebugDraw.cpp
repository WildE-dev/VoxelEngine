#include "render/DebugDraw.h"

#include <glm/gtc/matrix_transform.hpp>

#include "render/Shader.h"


DebugDraw::DebugDraw() {
    glGenVertexArrays(1, &outlineVao);
    glGenBuffers(1, &outlineVbo);
    glBindVertexArray(outlineVao);
    glBindBuffer(GL_ARRAY_BUFFER, outlineVbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

DebugDraw::~DebugDraw() {
    glDeleteVertexArrays(1, &outlineVao);
    glDeleteBuffers(1, &outlineVbo);
}

void DebugDraw::DrawOutline(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const glm::vec3& position,
                            const std::vector<glm::vec3>& segments, const glm::vec4& color, float scale) {
    if (segments.empty()) return;
    // Pushed out a hair from the block's centre so the lines sit just in front of the
    // faces they trace (and win the depth test against them), while anything really in
    // front of the block still hides them.
    // (Out from the middle of what's outlined, by the same small distance whatever its
    // size: a thin panel at the back of its cell, pushed out from the cell's middle,
    // would have its front face pushed in behind itself, and not show.)
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const auto& p : segments) lo = glm::min(lo, p), hi = glm::max(hi, p);
    const glm::vec3 middle = (lo + hi) * 0.5f, extent = glm::max(hi - lo, glm::vec3(1e-3f));
    const glm::vec3 grow = (extent + glm::vec3(scale - 1.0f)) / extent;
    glm::mat4 model = glm::translate(glm::mat4(1.0f), position + middle);
    model = glm::scale(model, grow);
    model = glm::translate(model, -middle);
    shader.Use();
    shader.SetUniform("model", model);
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("color", color);

    glBindVertexArray(outlineVao);
    glBindBuffer(GL_ARRAY_BUFFER, outlineVbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(segments.size() * sizeof(glm::vec3)), segments.data(), GL_DYNAMIC_DRAW);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(segments.size()));
    glDepthFunc(GL_LESS);
    glBindVertexArray(0);
}
