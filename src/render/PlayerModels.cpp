#include "render/PlayerModels.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "render/Shader.h"

namespace {

// Unit cube centred on the origin: 36 vertices of position + normal.
std::vector<float> CubeVertices() {
    const glm::vec3 normals[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    std::vector<float> v;
    for (const auto& n : normals) {
        // Two tangents spanning the face, ordered so the face winds counter-clockwise.
        const glm::vec3 a = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 b = glm::cross(n, a);
        const glm::vec3 c = n * 0.5f;
        const glm::vec3 corners[4] = { c - a * 0.5f - b * 0.5f, c + a * 0.5f - b * 0.5f, c + a * 0.5f + b * 0.5f, c - a * 0.5f + b * 0.5f };
        for (int i : { 0, 1, 2, 0, 2, 3 }) {
            v.insert(v.end(), { corners[i].x, corners[i].y, corners[i].z, n.x, n.y, n.z });
        }
    }
    return v;
}

constexpr float kWidth = 0.6f, kEyeHeight = 1.62f;
constexpr float kHead = 0.5f;

} // namespace

PlayerModels::PlayerModels() {
    const auto vertices = CubeVertices();
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
}

PlayerModels::~PlayerModels() {
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
}

glm::vec3 PlayerModels::ColorFor(int id) {
    static const glm::vec3 palette[] = {
        { 0.90f, 0.35f, 0.30f }, { 0.30f, 0.55f, 0.90f }, { 0.95f, 0.75f, 0.25f }, { 0.45f, 0.80f, 0.40f },
        { 0.75f, 0.45f, 0.85f }, { 0.30f, 0.80f, 0.80f }, { 0.95f, 0.55f, 0.75f }, { 0.85f, 0.85f, 0.85f },
    };
    return palette[static_cast<unsigned>(id) % (sizeof(palette) / sizeof(palette[0]))];
}

void PlayerModels::Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<PlayerModel>& players) {
    if (players.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    glBindVertexArray(vao);

    auto box = [&](const glm::mat4& model, const glm::vec3& color) {
        shader.SetUniform("model", model);
        shader.SetUniform("color", color);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    };

    for (const auto& p : players) {
        // Yaw follows the camera convention: 0 degrees looks along +x, 90 along +z.
        const glm::mat4 base = glm::rotate(glm::translate(glm::mat4(1.0f), p.feet), -glm::radians(p.yaw), glm::vec3(0, 1, 0));

        const float bodyHeight = kEyeHeight - kHead * 0.5f;
        box(glm::scale(glm::translate(base, glm::vec3(0, bodyHeight * 0.5f, 0)), glm::vec3(kWidth * 0.5f, bodyHeight, kWidth)), p.color);

        const glm::mat4 head = glm::rotate(glm::translate(base, glm::vec3(0, kEyeHeight, 0)), glm::radians(p.pitch), glm::vec3(0, 0, 1));
        box(glm::scale(head, glm::vec3(kHead)), p.color * 1.1f);
        // Visor on the front (+x in the head's frame).
        box(glm::scale(glm::translate(head, glm::vec3(kHead * 0.5f, 0.03f, 0)), glm::vec3(0.04f, kHead * 0.3f, kHead * 0.8f)), glm::vec3(0.1f));
    }
}
