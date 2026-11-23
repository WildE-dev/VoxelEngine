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
    // Where the rain stops in each column (none, to start).
    std::vector<float> open(static_cast<size_t>(kCoverSize) * kCoverSize, -1e6f);
    glGenTextures(1, &coverTexture);
    glBindTexture(GL_TEXTURE_2D, coverTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, kCoverSize, kCoverSize, 0, GL_RED, GL_FLOAT, open.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
}

Rain::~Rain() {
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteTextures(1, &coverTexture);
}

void Rain::Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const glm::vec3& cameraPosition, float time, bool snow,
                float intensity, float brightness, const float* cover) {
    if (intensity <= 0.0f) return;
    shader.Use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, coverTexture);
    if (cover) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kCoverSize, kCoverSize, GL_RED, GL_FLOAT, cover);
    shader.SetUniform("cover", 0);
    shader.SetUniform("coverSize", static_cast<float>(kCoverSize));
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("cameraPos", cameraPosition);
    shader.SetUniform("time", time);
    shader.SetUniform("intensity", intensity);
    shader.SetUniform("brightness", brightness);
    shader.SetUniform("radius", kRadius);
    shader.SetUniform("height", kHeight);
    shader.SetUniform("speed", snow ? kSpeed * 0.1f : kSpeed);
    shader.SetUniform("streak", snow ? 0.07f : kLength);
    shader.SetUniform("snow", snow ? 1.0f : 0.0f);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, kDrops * 2);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
