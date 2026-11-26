#pragma once
// A shovel's right-click: the ground round a block levelled to its height. Worked out
// here, from the blocks alone, so the server (which does it) and the client (which shows
// how far along it is while the button's held) agree on what it changes and how long
// it takes.
#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "world/BlockAccess.h"
#include "world/Inventory.h"
#include "world/Mining.h"

namespace Flatten {

constexpr int kMaxRadius = 3; // 7 by 7 (as net::kMaxFlattenRadius)

// Soft ground: what a shovel digs (dirt, grass, sand, snow, gravel), whole or shaped.
inline bool Soft(const Block& b) {
    return !b.IsAir() && !b.IsLiquid() && !b.IsPlant() && !IsModelType(b.type) && Mining::BestTool(b.type) == Tool::Shovel;
}

struct Change {
    glm::ivec3 at;
    Block to;
};

struct Plan {
    Block clicked;
    std::vector<Change> changes;   // bottom up, so what's filled in rests on what's under it
    std::map<ItemId, long> cost;   // units, by what they're made of: + taken, - given back
    int moved = 0;                 // units dug out and filled in, together
};

// What flattening round `pos` would do, or nothing if it isn't soft ground (or a block
// on its side) or there's nothing to level.
inline std::optional<Plan> Work(const BlockAccess& world, const glm::ivec3& pos, int radius) {
    radius = std::clamp(radius, 1, kMaxRadius);
    Plan plan;
    if (!world.GetBlock(pos.x, pos.y, pos.z, plan.clicked)) return std::nullopt;
    const Block& clicked = plan.clicked;
    if (!Soft(clicked) || clicked.ShapeDir() != 0) return std::nullopt;
    // The level: the clicked block's top (the middle of its corners, for a shaped one), in
    // sixteenths from the bottom of the world's rows.
    constexpr int kSteps = EdgeData::kSteps;
    int top = kSteps;
    if (!clicked.IsFullBlock()) {
        int sum = 0;
        for (int e = 0; e < 4; e++) sum += clicked.edgeData.GetTopY(e);
        top = (sum + 2) / 4;
    }
    const int level = pos.y * kSteps + top;
    for (int dx = -radius; dx <= radius; dx++)
        for (int dz = -radius; dz <= radius; dz++)
            for (int y = pos.y - 1; y <= pos.y + 2; y++) {
                const glm::ivec3 at(pos.x + dx, y, pos.z + dz);
                Block b;
                if (!world.GetBlock(at.x, at.y, at.z, b) || b.HasWater() || b.IsLava()) continue;
                const bool open = b.IsAir() || b.IsReplaceablePlant();
                if (!open && !(Soft(b) && b.ShapeDir() == 0)) continue; // (stone, wood, buildings: left be)
                const int fill = std::clamp(level - y * kSteps, 0, kSteps);
                // Filling an empty cell: with what was clicked (soft ground keeps its own kind).
                const BlockType kind = open ? clicked.type : b.type;
                Block to;
                if (fill == kSteps) to = Block(kind);
                else if (fill > 0) {
                    EdgeData edges;
                    for (int e = 0; e < 4; e++) edges.SetTopY(e, static_cast<uint8_t>(fill));
                    to = Block(kind, edges);
                }
                if (to == b || (open && fill == 0)) continue;
                const int before = open ? 0 : b.ResourceUnits(), after = to.ResourceUnits();
                if (before != after) plan.cost[open ? Items::ResourceOf(to.type) : Items::ResourceOf(b.type)] += after - before;
                plan.moved += std::abs(after - before);
                plan.changes.push_back({ at, to });
            }
    if (plan.changes.empty()) return std::nullopt;
    std::sort(plan.changes.begin(), plan.changes.end(), [](const Change& a, const Change& b) { return a.at.y < b.at.y; });
    return plan;
}

// How long it takes, in survival: a moment's work to start with, and then more for each
// block's worth of ground moved than digging that block out would take (it's levelled
// and packed down as well), so it's a tidier way to dig, not a faster one.
inline double Seconds(const Plan& plan, int tier) {
    const double dig = std::max(0.05f, Mining::Seconds(Block(plan.clicked.type), Tool::Shovel, tier));
    return dig * (1.0 + 1.5 * plan.moved / static_cast<double>(Block::kUnitsPerBlock));
}

} // namespace Flatten
