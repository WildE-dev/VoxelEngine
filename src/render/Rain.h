#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

class Shader;

// Falling rain streaks in a box around the camera. The drops are a fixed set of
// random points; the vertex shader moves them down over time and wraps them around
// the camera, so the box follows the player without the drops sliding with them.
// Intensity (0..1) decides how many of them show.
class Rain
{
public:
    Rain();
    ~Rain();

    Rain(const Rain&) = delete;
    Rain& operator=(const Rain&) = delete;

    // Into the current framebuffer, depth tested against the scene, blended.
    // `snow`: slow drifting flakes instead of streaks.
    void Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const glm::vec3& cameraPosition, float time, bool snow,
              float intensity, float brightness);

private:
    static constexpr int kDrops = 9000;
    GLuint vao = 0, vbo = 0;
};
