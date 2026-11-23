#pragma once
// Which pixels of each block texture are solid (not see-through), for aiming at
// plants by their actual picture (world/Raycast.cpp). The client sets them as it loads
// the textures (render/TextureAtlas.cpp); with none set (a server, most tests) plants
// are aimed at by a rough area of their sprite instead.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace SpriteMasks {

struct Masks {
    int size = 0;                              // pixels a side
    std::vector<std::vector<uint8_t>> solid;   // by texture index: size * size, row 0 the top
};
inline Masks& Get() {
    static Masks masks;
    return masks;
}

inline void Set(int size, std::vector<std::vector<uint8_t>> solid) {
    Get().size = size;
    Get().solid = std::move(solid);
}

// Whether the texture's pixel at (u, v) (0..1 across, 0..1 down from the top) is solid;
// nullopt when the textures aren't known.
inline std::optional<bool> Solid(int texture, float u, float v) {
    const Masks& m = Get();
    if (m.size <= 0 || texture < 0 || texture >= static_cast<int>(m.solid.size()) || m.solid[texture].empty()) return std::nullopt;
    const int x = std::min(m.size - 1, std::max(0, static_cast<int>(std::floor(u * m.size))));
    const int y = std::min(m.size - 1, std::max(0, static_cast<int>(std::floor(v * m.size))));
    return m.solid[texture][static_cast<size_t>(y * m.size + x)] != 0;
}

// The part of the texture with anything solid in it: across (u0..u1) and how high its
// top is up from the bottom (0..1). False when the textures aren't known (or it's empty).
inline bool Bounds(int texture, float& u0, float& u1, float& height) {
    const Masks& m = Get();
    if (m.size <= 0 || texture < 0 || texture >= static_cast<int>(m.solid.size()) || m.solid[texture].empty()) return false;
    int x0 = m.size, x1 = -1, y0 = m.size;
    for (int y = 0; y < m.size; y++)
        for (int x = 0; x < m.size; x++)
            if (m.solid[texture][static_cast<size_t>(y * m.size + x)]) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y);
    if (x1 < 0) return false;
    u0 = x0 / static_cast<float>(m.size);
    u1 = (x1 + 1) / static_cast<float>(m.size);
    height = 1.0f - y0 / static_cast<float>(m.size);
    return true;
}

} // namespace SpriteMasks
