#include "render/Rain.h"

#include <random>
#include <vector>

#include "render/Shader.h"

namespace {

// The box around the camera that drops live in, and how they fall (blocks, seconds).
constexpr float kRadius = 16.0f;
constexpr float kHeight = 24.0f;
constexpr float kSpeed = 16.0f;
constexpr float kLength = 1.1f;

} // namespace

Rain::Rain() {
    // Per vertex: position in the unit box (xyz) and which end of the streak (w: 0
    // bottom, 1 top); a per-drop random number decides when it shows (intensity).
    std::mt19937 random(1234);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::vector<float> data;
    data.reserve(kDrops * 2 * 5);
    for (int i = 0; i < kDrops; i++) {
        const float x = unit(random), y = unit(random), z = unit(random), r = unit(random);
        for (int end = 0; end < 2; end++) data.insert(data.end(), { x, y, z, static_cast<float>(end), r });
    }
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

Rain::~Rain() {
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
}

void Rain::Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const glm::vec3& cameraPosition, float time,
                float intensity, float brightness) {
    if (intensity <= 0.0f) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("cameraPos", cameraPosition);
    shader.SetUniform("time", time);
    shader.SetUniform("intensity", intensity);
    shader.SetUniform("brightness", brightness);
    shader.SetUniform("radius", kRadius);
    shader.SetUniform("height", kHeight);
    shader.SetUniform("speed", kSpeed);
    shader.SetUniform("streak", kLength);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, kDrops * 2);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
