#include "server/Steam.h"

#include <algorithm>

#include "world/BlockAccess.h"

bool SteamCloud::Open(const BlockAccess& world, const glm::ivec3& cell) {
    Block b;
    return world.GetBlock(cell.x, cell.y, cell.z, b) && (b.IsAir() || b.IsPlant()) && !b.HasWater();
}

void SteamCloud::Add(const glm::ivec3& cell, int units) {
    if (units <= 0) return;
    auto it = cells.find(cell);
    if (it == cells.end()) {
        if (cells.size() >= kMaxCells) return;
        it = cells.emplace(cell, 0).first;
    }
    it->second = static_cast<uint16_t>(std::min(kMaxCell, it->second + units));
}

long long SteamCloud::Total() const {
    long long n = 0;
    for (const auto& [cell, units] : cells) n += units;
    return n;
}

void SteamCloud::Update(const BlockAccess& world) {
    if (cells.empty()) return;
    // How much of the difference goes each way: up, each side, down.
    struct Way {
        glm::ivec3 d;
        int perThousand;
    };
    static const Way kWays[6] = { { { 0, 1, 0 }, 300 }, { { 1, 0, 0 }, 110 }, { { -1, 0, 0 }, 110 },
                                  { { 0, 0, 1 }, 110 }, { { 0, 0, -1 }, 110 }, { { 0, -1, 0 }, 30 } };
    // (A crowded world fades faster.)
    const int fade = kFadePerThousand * (cells.size() > kMaxCells * 3 / 4 ? 3 : 1);
    Cells next;
    next.reserve(cells.size() * 2);
    // Whether each cell's open, asked of the world once a step (cells and their neighbours
    // are asked about over and over otherwise).
    std::unordered_map<glm::ivec3, bool, Coords::IVec3Hash> open;
    open.reserve(cells.size() * 3);
    auto isOpen = [&](const glm::ivec3& c) {
        auto it = open.find(c);
        if (it != open.end()) return it->second;
        return open[c] = Open(world, c);
    };
    for (const auto& [cell, units] : cells) {
        int left = units;
        if (!isOpen(cell)) {
            // Something filled its cell: up out of the way (or gone, if it can't).
            const glm::ivec3 up = cell + glm::ivec3(0, 1, 0);
            if (isOpen(up)) next[up] = static_cast<uint16_t>(std::min(kMaxCell, next[up] + left));
            continue;
        }
        left -= left * fade / 1000 + (left < 40 ? 1 : 0); // (thin wisps go, a unit at a time)
        if (left <= 0) continue;
        const int start = left;
        for (const Way& w : kWays) {
            const glm::ivec3 n = cell + w.d;
            const int theirs = At(n);
            if (theirs >= start || !isOpen(n)) continue;
            // (Thick steam rises hard; thin steam barely, hanging and spreading out.)
            const int share = w.d.y > 0 ? 20 + w.perThousand * std::min(start, 400) / 400 : w.perThousand;
            const int give = (start - theirs) * share / 1000;
            if (give <= 0) continue;
            next[n] = static_cast<uint16_t>(std::min(kMaxCell, next[n] + give));
            left -= give;
        }
        next[cell] = static_cast<uint16_t>(std::min(kMaxCell, next[cell] + std::max(0, left)));
    }
    cells.clear();
    for (const auto& [cell, units] : next)
        if (units >= kMin && cells.size() < kMaxCells) cells.emplace(cell, units);
}
