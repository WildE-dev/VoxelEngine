#pragma once
// A chunk mesh's vertex: two 32-bit words, read by Resources/Shaders/main.vert and
// water.vert (which must match).
//   position: x, y, z in sixteenths of a block within the chunk (9 bits each; y stored
//             a block up, so plant sprites can reach below the chunk's floor), then the
//             texture's u in sixteenths (5 bits: 0..16)
//               x << 23 | y << 14 | z << 5 | u
//   shading:  the texture's v (5 bits), the normal (8 bits: octahedral, 4 bits an axis),
//             ambient occlusion (2 bits; water keeps its surface's fine height there),
//             block light and sky light (4 bits each), the texture's index (9 bits)
//               v << 27 | normal << 19 | ao << 17 | light << 13 | sky << 9 | texture
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>

namespace MeshVertex {

constexpr float kUnits = 16.0f;             // positions and texture coordinates in sixteenths
constexpr uint32_t kAoShift = 17;           // (water.vert reads its fine height here)
constexpr uint32_t kMaxTextures = 1u << 9;  // the atlas index's room

inline uint32_t Quantise(float v, uint32_t mask) { return static_cast<uint32_t>(std::lround(v * kUnits)) & mask; }

inline uint32_t PackPosition(const glm::vec3& pos, float u) {
    return (Quantise(pos.x, 0x1FFu) << 23) | (Quantise(pos.y + 1.0f, 0x1FFu) << 14) | (Quantise(pos.z, 0x1FFu) << 5) | Quantise(u, 0x1Fu);
}
inline glm::vec3 UnpackPosition(uint32_t p) {
    return glm::vec3(static_cast<float>((p >> 23) & 0x1FFu), static_cast<float>((p >> 14) & 0x1FFu), static_cast<float>((p >> 5) & 0x1FFu)) / kUnits -
           glm::vec3(0.0f, 1.0f, 0.0f);
}
// The same word with a new position (its u kept).
inline uint32_t WithPosition(uint32_t p, const glm::vec3& pos) { return (PackPosition(pos, 0.0f) & ~0x1Fu) | (p & 0x1Fu); }

// Octahedral normal, 4 bits an axis: 0..14 (7 is 0), so the axes are exact.
inline uint32_t PackNormal(glm::vec3 n) {
    const float l = std::abs(n.x) + std::abs(n.y) + std::abs(n.z);
    if (l < 1e-6f) return (7u << 4) | 7u;
    n /= l;
    glm::vec2 o(n.x, n.y);
    if (n.z < 0.0f) o = glm::vec2((1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f), (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f));
    auto q = [](float v) { return static_cast<uint32_t>(std::clamp(std::lround((v + 1.0f) * 7.0f), 0L, 14L)); };
    return (q(o.x) << 4) | q(o.y);
}
inline glm::vec3 UnpackNormal(uint32_t bits) {
    glm::vec2 f(static_cast<float>((bits >> 4) & 0xFu) / 7.0f - 1.0f, static_cast<float>(bits & 0xFu) / 7.0f - 1.0f);
    glm::vec3 n(f.x, f.y, 1.0f - std::abs(f.x) - std::abs(f.y));
    if (n.z < 0.0f) n = glm::vec3((1.0f - std::abs(f.y)) * (f.x >= 0.0f ? 1.0f : -1.0f), (1.0f - std::abs(f.x)) * (f.y >= 0.0f ? 1.0f : -1.0f), n.z);
    return glm::normalize(n);
}

inline uint32_t PackShading(float v, const glm::vec3& normal, uint32_t ao, uint32_t light, uint32_t sky, int texture) {
    return (Quantise(v, 0x1Fu) << 27) | (PackNormal(normal) << 19) | ((ao & 0x3u) << kAoShift) | ((light & 0xFu) << 13) | ((sky & 0xFu) << 9) |
           (static_cast<uint32_t>(texture) & (kMaxTextures - 1));
}
inline glm::vec3 ShadingNormal(uint32_t s) { return UnpackNormal((s >> 19) & 0xFFu); }
inline uint32_t WithNormal(uint32_t s, const glm::vec3& n) { return (s & ~(0xFFu << 19)) | (PackNormal(n) << 19); }

} // namespace MeshVertex
