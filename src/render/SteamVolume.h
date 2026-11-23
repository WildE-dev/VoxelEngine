#pragma once
// Steam in the air (server/Steam.h), drawn as a translucent cloud filling the cells it's
// in: each face between a cell of steam and its neighbours is a see-through square, as
// misty as the steam beside it is thick, and looking through more steam goes through
// more of them, so a thick cloud is near white and its thin edges barely there. The
// shader breaks it up with drifting noise, so it reads as cloud, not glass.
#include <cstdint>
#include <vector>

#include <glad/glad.h>
#include <glm/glm.hpp>

class Shader;

class SteamVolume
{
public:
    SteamVolume();
    ~SteamVolume();
    SteamVolume(const SteamVolume&) = delete;
    SteamVolume& operator=(const SteamVolume&) = delete;

    // The cells of steam (xyz: the cell, w: how thick, 0..255), remeshed when `version`
    // changes.
    void Update(const std::vector<glm::vec4>& cells, uint32_t version);
    // Into the current framebuffer, depth tested against the scene but not written, blended.
    void Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time, float brightness);

private:
    GLuint vao = 0, vbo = 0;
    GLsizei vertexCount = 0;
    uint32_t meshed = UINT32_MAX;
};
