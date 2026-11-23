#include "render/SteamVolume.h"

#include <cmath>
#include <unordered_map>

#include "render/Shader.h"
#include "world/Coords.h"

namespace {
// How misty one face is for steam this thick (0..255): a cell's worth of it.
float Mist(float density) { return 1.0f - std::exp(-1.6f * density / 255.0f); }
} // namespace

SteamVolume::SteamVolume() {
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
}

SteamVolume::~SteamVolume() {
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
}

void SteamVolume::Update(const std::vector<glm::vec4>& cells, uint32_t version) {
    if (version == meshed) return;
    meshed = version;
    std::unordered_map<glm::ivec3, float, Coords::IVec3Hash> density;
    density.reserve(cells.size() * 2);
    for (const auto& c : cells) density[glm::ivec3(c)] = c.w;
    std::vector<float> data;
    data.reserve(cells.size() * 6 * 6 * 4);
    // How thick the steam is at a corner of the grid: the mean of the eight cells round it
    // (none counting as nothing). Faces take their mist from their corners, so it fades
    // smoothly across them and to nothing at the cloud's edge: the cells blend together
    // instead of showing as boxes.
    std::unordered_map<glm::ivec3, float, Coords::IVec3Hash> corners;
    auto corner = [&](const glm::ivec3& p) {
        auto it = corners.find(p);
        if (it != corners.end()) return it->second;
        float sum = 0.0f;
        for (int dx = -1; dx <= 0; dx++)
            for (int dy = -1; dy <= 0; dy++)
                for (int dz = -1; dz <= 0; dz++)
                    if (auto c = density.find(p + glm::ivec3(dx, dy, dz)); c != density.end()) sum += c->second;
        return corners[p] = sum / 8.0f;
    };
    // A square across the cell's face on `axis`, at `at` (its low corner), misty by its
    // corners (`scale`: how much of each corner's steam it shows; shared faces carry the
    // steam on both sides).
    auto face = [&](const glm::ivec3& at, int axis, float scale) {
        const int u = (axis + 1) % 3, v = (axis + 2) % 3;
        glm::ivec3 p[4] = { at, at, at, at };
        p[1][u] += 1;
        p[2][u] += 1, p[2][v] += 1;
        p[3][v] += 1;
        float a[4];
        bool any = false;
        for (int i = 0; i < 4; i++) any |= (a[i] = Mist(corner(p[i]) * scale)) >= 0.01f;
        if (!any) return;
        for (const int i : { 0, 1, 2, 0, 2, 3 }) data.insert(data.end(), { static_cast<float>(p[i].x), static_cast<float>(p[i].y), static_cast<float>(p[i].z), a[i] });
    };
    for (const auto& [cell, d] : density) {
        for (int axis = 0; axis < 3; axis++) {
            glm::ivec3 low = cell, high = cell;
            low[axis] -= 1;
            high[axis] += 1;
            // The face toward -axis: shared with steam there (each shared face once, as
            // misty as the two cells' steam), or the cloud's edge.
            (void)d;
            face(cell, axis, density.count(low) ? 1.0f : 1.5f);
            // The face toward +axis, only at the cloud's edge (shared ones are the other's).
            if (!density.count(high)) {
                glm::ivec3 at = cell;
                at[axis] += 1;
                face(at, axis, 1.5f);
            }
        }
    }
    vertexCount = static_cast<GLsizei>(data.size() / 4);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.size() * sizeof(float)), data.empty() ? nullptr : data.data(), GL_DYNAMIC_DRAW);
}

void SteamVolume::Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection, float time, float brightness) {
    if (vertexCount == 0) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("time", time);
    shader.SetUniform("brightness", brightness);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE); // (seen from inside the cloud too)
    // Its faces lie on the cells' boundaries, where a block's face may be too: pushed a
    // touch further away, so the block's face always wins there (no flickering).
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 4.0f);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
    glBindVertexArray(0);
    glPolygonOffset(0.0f, 0.0f);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
