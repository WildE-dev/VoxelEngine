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

inline int FloorDiv(int a, int b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

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

// A rake's right-click: the ground round `pos` smoothed, its steps made slopes. Each
// column's ground is read as a height (the top of its highest block, in sixteenths), and
// each corner where columns meet goes to the average of the columns round it, as the
// world's own hills are shaped (TerrainGenerator's MakeColumn): a block-high step becomes
// a slope across the columns either side of it. Only soft ground (Soft) is moved; stone,
// wood and buildings keep their shape, but the ground beside them meets their height.
// Columns with water (or lava) in them are left be.
inline std::optional<Plan> Smooth(const BlockAccess& world, const glm::ivec3& pos, int radius) {
    radius = std::clamp(radius, 1, kMaxRadius);
    Plan plan;
    if (!world.GetBlock(pos.x, pos.y, pos.z, plan.clicked) || !Soft(plan.clicked) || plan.clicked.ShapeDir() != 0) return std::nullopt;
    constexpr int kSteps = EdgeData::kSteps;
    constexpr int kUp = 3, kDown = 4;  // how far above and below the block clicked a column's ground is looked for
    const int n = 2 * radius + 3;      // the columns changed, and one more each side, read only
    struct Column {
        bool known = false, soft = false;
        int row = 0;                   // its top block's row
        int height = 0;                // the ground's height, in sixteenths from the world's row 0
        Block top;
    };
    std::vector<Column> columns(n * n);
    auto column = [&](int i, int k) -> Column& { return columns[i + n * k]; };
    auto open = [](const Block& b) { return b.IsAir() || b.IsReplaceablePlant(); };
    for (int i = 0; i < n; i++)
        for (int k = 0; k < n; k++) {
            const int x = pos.x - radius - 1 + i, z = pos.z - radius - 1 + k;
            Column& c = column(i, k);
            bool wet = false;
            for (int y = pos.y + kUp; y >= pos.y - kDown; y--) {
                Block b;
                if (!world.GetBlock(x, y, z, b)) break;
                if (b.HasWater() || b.IsLava()) {
                    wet = true;
                    break;
                }
                if (open(b)) continue;
                c.known = true;
                c.row = y;
                c.top = b;
                c.soft = Soft(b) && b.ShapeDir() == 0;
                int top = kSteps;
                if (!b.IsFullBlock()) {
                    int sum = 0;
                    for (int e = 0; e < 4; e++) sum += b.edgeData.GetTopY(e);
                    top = (sum + 2) / 4;
                }
                c.height = y * kSteps + top;
                break;
            }
            if (wet) c.known = c.soft = false;
        }
    // A corner's height: the average of the (known) columns round it. Corner (ci, ck) is
    // the one at the low x, low z of column (ci, ck).
    auto corner = [&](int ci, int ck, int& out) {
        int sum = 0, count = 0;
        for (int di = -1; di <= 0; di++)
            for (int dk = -1; dk <= 0; dk++) {
                const int i = ci + di, k = ck + dk;
                if (i < 0 || k < 0 || i >= n || k >= n || !column(i, k).known) continue;
                sum += column(i, k).height;
                count++;
            }
        if (count == 0) return false;
        out = FloorDiv(2 * sum + count, 2 * count); // (rounded)
        return true;
    };
    for (int i = 1; i < n - 1; i++)
        for (int k = 1; k < n - 1; k++) {
            const Column& c = column(i, k);
            if (!c.known || !c.soft) continue;
            const int x = pos.x - radius - 1 + i, z = pos.z - radius - 1 + k;
            // EdgeData order: 0 = (x, z+1), 1 = (x+1, z), 2 = (x, z), 3 = (x+1, z+1).
            int h[4];
            if (!corner(i, k + 1, h[0]) || !corner(i + 1, k, h[1]) || !corner(i, k, h[2]) || !corner(i + 1, k + 1, h[3])) continue;
            // Its surface: one shaped block, in the row holding the corners' average (a
            // mean on a row's line counts as the top of the row under it); corners above
            // that row come down to its top, those below it up to a pixel over its floor.
            const int mean = FloorDiv(h[0] + h[1] + h[2] + h[3] + 2, 4);
            const int row = FloorDiv(mean - 1, kSteps);
            if (row < pos.y - kDown || row > pos.y + kUp) continue;
            EdgeData edges;
            bool whole = true;
            for (int e = 0; e < 4; e++) {
                const int up = std::clamp(h[e] - row * kSteps, 1, kSteps);
                whole &= up == kSteps;
                edges.Set(e, up, 0);
            }
            // What it's made of: the top keeps its kind (grass stays grass); ground filled
            // in under it is the same, but dirt under grass.
            const BlockType kind = c.top.type;
            const BlockType under = IsGrassType(kind) ? BlockType::DIRT : kind;
            std::vector<Change> column;
            bool blocked = false;
            const int lo = std::min(row, c.row), hi = std::max(row, c.row);
            for (int y = lo; y <= hi && !blocked; y++) {
                Block b;
                if (!world.GetBlock(x, y, z, b)) {
                    blocked = true;
                    break;
                }
                Block to;
                if (y < row) to = open(b) ? Block(under) : (y == c.row ? Block(IsGrassType(b.type) ? BlockType::DIRT : b.type) : b);
                else if (y == row) to = whole ? Block(kind) : Block(kind, edges);
                // (above the new surface: cleared)
                if (to == b) continue;
                if (!open(b) && !(Soft(b) && b.ShapeDir() == 0)) blocked = true; // (something not to dig through)
                else column.push_back({ glm::ivec3(x, y, z), to });
            }
            if (blocked) continue;
            for (const Change& ch : column) {
                Block b;
                world.GetBlock(ch.at.x, ch.at.y, ch.at.z, b);
                const bool was = !open(b);
                const int before = was ? b.ResourceUnits() : 0, after = ch.to.ResourceUnits();
                // (Grass and dirt are the same ground here: grass buried turns to dirt,
                // and a surface moved keeps its grass, at no cost in grass.)
                auto earth = [](BlockType t) { return Items::ResourceOf(IsGrassType(t) ? BlockType::DIRT : t); };
                if (was) plan.cost[earth(b.type)] -= before;
                if (after) plan.cost[earth(ch.to.type)] += after;
                plan.moved += std::abs(after - before);
                plan.changes.push_back(ch);
            }
        }
    for (auto it = plan.cost.begin(); it != plan.cost.end();) it = it->second == 0 ? plan.cost.erase(it) : std::next(it);
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
