// Sand and gravel (world/Grains.h), stepped in a little world of their own (stone below
// y 0, open above, inside a box). Checks that
//  - sand in the air falls to the ground, and a tower of it spreads into a pile, none
//    lost or made, no side steeper than sand stands, nothing hanging in the air;
//  - the pile is drawn with slopes (its corners follow the ground round it);
//  - gravel stands steeper (a taller pile from the same amount);
//  - a shape on its side, or cut from below, settles upright, keeping what it holds;
//  - sand falls through water, and the water rises into the space, none lost;
//  - a pile at rest stays at rest (a step changes nothing).
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>

#include "world/Grains.h"

namespace {

int failures = 0;
void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

constexpr int kBox = 24; // x and z in -kBox..kBox, y in -2..30

class MiniWorld : public BlockAccess {
public:
    std::unordered_map<glm::ivec3, Block, Coords::IVec3Hash> blocks;
    bool GetBlock(int x, int y, int z, Block& block) const override {
        if (x < -kBox || x > kBox || z < -kBox || z > kBox || y < -2 || y > 30) return false;
        auto it = blocks.find({ x, y, z });
        if (it != blocks.end()) block = it->second;
        else block = y < 0 ? Block(BlockType::STONE) : Block();
        return true;
    }
    void Set(const glm::ivec3& p, const Block& b) {
        if (b == Block() && p.y >= 0) blocks.erase(p);
        else blocks[p] = b;
    }
    Block At(const glm::ivec3& p) const {
        Block b;
        GetBlock(p.x, p.y, p.z, b);
        return b;
    }

    // Steps until nothing changes (or `limit` steps), waking what's round each change
    // as the server does. Returns the steps taken.
    int Settle(std::vector<glm::ivec3> woken, int limit = 2000) {
        for (int step = 0; step < limit; step++) {
            std::vector<Grains::Change> changes;
            Grains::Step(*this, woken, changes, static_cast<uint32_t>(step));
            if (changes.empty()) return step;
            std::unordered_set<glm::ivec3, Coords::IVec3Hash> next;
            for (const auto& c : changes) {
                Set(c.pos, c.block);
                for (int dx = -1; dx <= 1; dx++)
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dz = -1; dz <= 1; dz++) {
                            const glm::ivec3 n = c.pos + glm::ivec3(dx, dy, dz);
                            if (Grains::IsGrain(At(n))) next.insert(n);
                        }
            }
            woken.assign(next.begin(), next.end());
        }
        return limit;
    }

    int Total(BlockType type) const {
        int sum = 0;
        for (const auto& [p, b] : blocks)
            if (b.type == type) sum += Grains::Amount(b);
        return sum;
    }
    int Water() const {
        int sum = 0;
        for (const auto& [p, b] : blocks) sum += b.WaterVolume();
        return sum;
    }
    // A column's ground height (sixths of a sixteenth above y 0) of this type.
    int Height(int x, int z, BlockType type) const {
        int h = 0;
        for (int y = 0; y <= 30; y++) {
            const Block b = At({ x, y, z });
            if (b.type == type) h = y * Grains::kFull + Grains::Amount(b);
        }
        return h;
    }
};

std::vector<glm::ivec3> Tower(MiniWorld& w, BlockType type, int height, int bottom = 0) {
    std::vector<glm::ivec3> cells;
    for (int y = bottom; y < bottom + height; y++) {
        w.Set({ 0, y, 0 }, Block(type));
        cells.push_back({ 0, y, 0 });
    }
    return cells;
}

// The steepest step between columns side by side, and whether anything hangs in the air
// or isn't upright.
struct PileShape {
    int steepest = 0, peak = 0, columns = 0;
    bool floating = false, notUpright = false, sloped = false;
};
PileShape Look(const MiniWorld& w, BlockType type) {
    PileShape s;
    for (int x = -kBox; x <= kBox; x++)
        for (int z = -kBox; z <= kBox; z++) {
            const int h = w.Height(x, z, type);
            if (h > 0) s.columns++;
            s.peak = std::max(s.peak, h);
            for (const auto& d : { glm::ivec2(1, 0), glm::ivec2(0, 1) }) {
                const int other = w.Height(x + d.x, z + d.y, type);
                if (h > 0 || other > 0) s.steepest = std::max(s.steepest, std::abs(h - other));
            }
        }
    for (const auto& [p, b] : w.blocks) {
        if (b.type != type) continue;
        const Block under = w.At(p + glm::ivec3(0, -1, 0));
        if (under.IsAir()) s.floating = true;
        if (!Grains::Upright(b)) s.notUpright = true;
        if (b.IsPartial()) {
            int lo = 16, hi = 0;
            for (int e = 0; e < 4; e++) lo = std::min<int>(lo, b.edgeData.GetTopY(e)), hi = std::max<int>(hi, b.edgeData.GetTopY(e));
            if (hi > lo) s.sloped = true;
        }
    }
    return s;
}

} // namespace

int main() {
    std::cout << "Shapes" << std::endl;
    {
        // Whatever the corners are wanted at, the shape holds exactly what it's asked to.
        uint32_t seed = 12345;
        auto next = [&] { return (seed = seed * 1664525u + 1013904223u) >> 8; };
        int wrong = 0, tries = 0;
        for (int amount = Grains::kMin; amount < Grains::kFull; amount++)
            for (int i = 0; i < 200; i++, tries++) {
                std::array<int, 4> want;
                for (int& h : want) h = static_cast<int>(next() % 40) - 12;
                const Block b = Grains::Shape(BlockType::SAND, amount, want);
                if (Grains::Amount(b) != amount || !Grains::Upright(b)) wrong++;
            }
        Check(wrong == 0, "a shape holds exactly what it's asked to, however its corners are wanted (" + std::to_string(wrong) + " of " + std::to_string(tries) + " wrong)");
    }

    std::cout << "Falling" << std::endl;
    {
        MiniWorld w;
        w.Set({ 3, 6, 3 }, Block(BlockType::SAND));
        w.Settle({ { 3, 6, 3 } });
        Check(w.At({ 3, 0, 3 }).type == BlockType::SAND && w.At({ 3, 6, 3 }).IsAir() && w.Total(BlockType::SAND) == Grains::kFull,
              "a block of sand in the air falls to the ground");
    }

    std::cout << "Piles" << std::endl;
    int sandPeak = 0;
    {
        MiniWorld w;
        const auto cells = Tower(w, BlockType::SAND, 12);
        const int before = w.Total(BlockType::SAND);
        const int steps = w.Settle(cells);
        const PileShape s = Look(w, BlockType::SAND);
        sandPeak = s.peak;
        Check(steps < 2000, "a tower of 12 sand comes to rest (" + std::to_string(steps) + " steps)");
        Check(w.Total(BlockType::SAND) == before, "with none lost or made (" + std::to_string(w.Total(BlockType::SAND)) + " of " + std::to_string(before) + ")");
        Check(s.peak < 6 * Grains::kFull && s.columns > 9,
              "spread into a pile: " + std::to_string(s.peak / 96.0f).substr(0, 4) + " blocks high over " + std::to_string(s.columns) + " columns");
        Check(s.steepest <= Grains::Repose(BlockType::SAND) + Grains::kSixths,
              "no side steeper than sand stands (" + std::to_string(s.steepest) + " <= " + std::to_string(Grains::Repose(BlockType::SAND)) + ")");
        Check(!s.floating && !s.notUpright, "nothing hanging in the air, every cell upright");
        Check(s.sloped, "drawn with slopes: its corners follow the ground round it");
        std::vector<glm::ivec3> all;
        for (const auto& [p, b] : w.blocks)
            if (Grains::IsGrain(b)) all.push_back(p);
        std::vector<Grains::Change> changes;
        Grains::Step(w, all, changes, 7);
        Check(changes.empty(), "and at rest it stays so: a step changes nothing (" + std::to_string(changes.size()) + " changes)");
    }
    {
        MiniWorld w;
        const auto cells = Tower(w, BlockType::GRAVEL, 12);
        w.Settle(cells);
        const PileShape s = Look(w, BlockType::GRAVEL);
        Check(s.peak > sandPeak && w.Total(BlockType::GRAVEL) == 12 * Grains::kFull,
              "gravel stands steeper: a taller pile from as much (" + std::to_string(s.peak) + " > " + std::to_string(sandPeak) + ")");
    }

    std::cout << "Upright" << std::endl;
    {
        MiniWorld w;
        EdgeData edges;
        for (int e = 0; e < 4; e++) edges.Set(e, 8, 0);
        Block side(BlockType::SAND, edges);
        side.SetShapeDir(1); // a half slab on its side
        w.Set({ 0, 0, 0 }, side);
        const int units = side.ResourceUnits();
        w.Settle({ { 0, 0, 0 } });
        const Block b = w.At({ 0, 0, 0 });
        Check(b.type == BlockType::SAND && Grains::Upright(b) && b.ResourceUnits() == units,
              "sand on its side settles upright, keeping what it holds (" + std::to_string(b.ResourceUnits()) + " of " + std::to_string(units) + " units)");
        EdgeData under;
        for (int e = 0; e < 4; e++) under.Set(e, 16, 6);
        w.Set({ 4, 0, 4 }, Block(BlockType::GRAVEL, under)); // cut from below
        const int cut = w.At({ 4, 0, 4 }).ResourceUnits();
        w.Settle({ { 4, 0, 4 } });
        const Block g = w.At({ 4, 0, 4 });
        Check(g.type == BlockType::GRAVEL && Grains::Upright(g) && g.ResourceUnits() == cut, "gravel cut from below drops to the floor, as much as it was");
    }

    std::cout << "Water" << std::endl;
    {
        MiniWorld w;
        // A pond a block deep in a stone basin, sand above it.
        for (int x = -1; x <= 1; x++)
            for (int z = -1; z <= 1; z++) w.Set({ x, -1, z }, Block::Water(Block::kMaxWaterVolume));
        w.Set({ 0, 2, 0 }, Block(BlockType::SAND));
        const int water = w.Water();
        w.Settle({ { 0, 2, 0 } });
        Check(w.At({ 0, -1, 0 }).type == BlockType::SAND && w.Water() == water, "sand falls through water to the bottom, and the water rises, none lost");
    }

    std::cout << (failures ? "SOME GRAIN CHECKS FAILED (" : "ALL GRAIN CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
