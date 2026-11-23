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
                            const std::vector<glm::vec3>& segments) {
    if (segments.empty()) return;
    // Pushed out a hair from the block's centre so the lines sit just in front of the
    // faces they trace (and win the depth test against them), while anything really in
    // front of the block still hides them.
    const glm::vec3 centre = position + glm::vec3(0.5f);
    glm::mat4 model = glm::translate(glm::mat4(1.0f), centre);
    model = glm::scale(model, glm::vec3(1.004f));
    model = glm::translate(model, -glm::vec3(0.5f));
    shader.Use();
    shader.SetUniform("model", model);
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);

    glBindVertexArray(outlineVao);
    glBindBuffer(GL_ARRAY_BUFFER, outlineVbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(segments.size() * sizeof(glm::vec3)), segments.data(), GL_DYNAMIC_DRAW);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(segments.size()));
    glDepthFunc(GL_LESS);
    glBindVertexArray(0);
}
