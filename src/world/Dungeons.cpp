#include "world/Dungeons.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "world/Noise.h"
#include "world/TerrainGenerator.h"

namespace Dungeons {

namespace {

constexpr int S = Chunk::CHUNK_SIZE;
constexpr uint32_t kSeed = 0xD06E0A11u;
constexpr float kDungeonChance = 0.3f; // about one region in three
// How far above the ground the entrance shaft clears trees and plants away.
constexpr int kShaftClearance = 12;

int FloorDiv(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// Small deterministic generator for layouts (std distributions differ between
// standard libraries, which would change the world from one build to another).
struct Rng {
    uint32_t state;
    uint32_t Next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    int Range(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<uint32_t>(hi - lo + 1)); } // inclusive
    float Unit() { return (Next() >> 8) / float(1u << 24); }
};

Box Expand(const Box& b, int by) { return { b.min - by, b.max + by }; }
Box Union(const Box& a, const Box& b) { return { glm::min(a.min, b.min), glm::max(a.max, b.max) }; }
bool Overlaps(const Box& a, const Box& b) { return glm::all(glm::lessThanEqual(a.min, b.max)) && glm::all(glm::lessThanEqual(b.min, a.max)); }
glm::ivec3 Centre(const Box& b) { return (b.min + b.max) / 2; }

// Height of a room's open space by type (the pumps and pillars need room).
int HeightFor(RoomType type) {
    switch (type) {
    case RoomType::Hall:
    case RoomType::PumpRoom: return 6;
    case RoomType::Crypt: return 4;
    default: return 5;
    }
}

// Whether a type fits a room of this footprint.
bool Fits(RoomType type, int w, int d) {
    switch (type) {
    case RoomType::Pool:
    case RoomType::Cistern:
    case RoomType::Hall: return w >= 9 && d >= 9;
    case RoomType::PumpRoom: return w >= 11 && d >= 7;
    case RoomType::Archive:
    case RoomType::Shrine:
    case RoomType::FloodChamber: return w >= 7 && d >= 7;
    default: return true;
    }
}

// Sunken parts of rooms: a cistern's pit, a pool, a pump room's lower basin.
Box CisternPit(const Room& r) { return { { r.inner.min.x + 2, r.inner.min.y - 4, r.inner.min.z + 2 }, { r.inner.max.x - 2, r.inner.min.y - 1, r.inner.max.z - 2 } }; }
Box PoolBasin(const Room& r) { return { { r.inner.min.x + 2, r.inner.min.y - 2, r.inner.min.z + 2 }, { r.inner.max.x - 2, r.inner.min.y - 1, r.inner.max.z - 2 } }; }
// Pump room geometry runs from the basin end (offset 0 is the open column next to
// that wall) toward the tub end; `flip` puts the basin at the high-x end.
int PumpX(const Room& r, int offset) { return r.flip ? r.inner.max.x - offset : r.inner.min.x + offset; }
int PumpLength(const Room& r) { return r.inner.max.x - r.inner.min.x + 1; }
Box Span(int xa, int xb, int y0, int y1, int z0, int z1) { return { { std::min(xa, xb), y0, z0 }, { std::max(xa, xb), y1, z1 } }; }
Box PumpBasin(const Room& r) {
    return Span(PumpX(r, 1), PumpX(r, 3), r.inner.min.y - 2, r.inner.min.y - 1, r.lane - 1, r.lane + 1);
}
// The raised tub with its rim (it stands on the floor, four blocks high).
Box PumpTub(const Room& r) {
    const int w = PumpLength(r);
    return Span(PumpX(r, w - 5), PumpX(r, w - 1), r.inner.min.y, r.inner.min.y + 3, r.lane - 2, r.lane + 2);
}
// The spike pit round a vault's pedestal (under its floor), and a flood chamber's
// reservoir (above its ceiling).
Box VaultPit(const Room& r) {
    const glm::ivec3 c = Centre(r.inner);
    return { { c.x - 1, r.inner.min.y - 3, c.z - 1 }, { c.x + 1, r.inner.min.y - 2, c.z + 1 } };
}
Box Reservoir(const Room& r) {
    return { { r.inner.min.x + 1, r.inner.max.y + 2, r.inner.min.z + 1 }, { r.inner.max.x - 1, r.inner.max.y + 4, r.inner.max.z - 1 } };
}
// A flood chamber's latch gate (in the ceiling, beside the sluice gates) and the
// lever that resets it (in the wall, at head height, by a corner).
glm::ivec3 FloodLatch(const Room& r) {
    const glm::ivec3 c = Centre(r.inner);
    return { c.x + 2, r.inner.max.y + 1, c.z };
}
glm::ivec3 FloodLever(const Room& r) { return { r.inner.min.x - 1, r.inner.min.y + 1, r.inner.min.z + 1 }; }
// A room's extra open part beyond its main space (sunken or raised), if it has one: it
// gets a shell and is hollowed out with the room.
bool Annex(const Room& r, Box& out) {
    switch (r.type) {
    case RoomType::Cistern: out = CisternPit(r); return true;
    case RoomType::Pool: out = PoolBasin(r); return true;
    case RoomType::PumpRoom: out = PumpBasin(r); return true;
    case RoomType::Vault: out = VaultPit(r); return true;
    case RoomType::FloodChamber: out = Reservoir(r); return true;
    default: return false;
    }
}

// Writes into one chunk, ignoring anything outside it. Furnishings may only read
// (IsWall) the cell they're about to change: every chunk must make the same decision
// for a cell, and a neighbouring cell may be in a chunk that isn't here.
struct Stamper {
    glm::ivec3 origin;
    std::array<Block, Chunk::VOLUME>& out;
    Box chunk;

    bool In(const glm::ivec3& p) const { return chunk.Contains(p); }
    Block& At(const glm::ivec3& p) {
        const glm::ivec3 l = p - origin;
        return out[Chunk::Index(l.x, l.y, l.z)];
    }
    void Set(const glm::ivec3& p, const Block& b) {
        if (In(p)) At(p) = b;
    }
    bool IsWall(const glm::ivec3& p) {
        if (!In(p)) return false;
        const BlockType t = At(p).type;
        return t == BlockType::STONE_BRICK || t == BlockType::MOSSY_BRICK;
    }
    // Only over dungeon wall: keeps doorways (and anything else) open.
    void SetIfWall(const glm::ivec3& p, const Block& b) {
        if (IsWall(p)) At(p) = b;
    }
    template <typename Fn>
    void Fill(const Box& box, Fn fn) {
        const glm::ivec3 lo = glm::max(box.min, chunk.min), hi = glm::min(box.max, chunk.max);
        for (int x = lo.x; x <= hi.x; x++)
            for (int y = lo.y; y <= hi.y; y++)
                for (int z = lo.z; z <= hi.z; z++) fn(glm::ivec3(x, y, z));
    }
};

// Bricks, some of them mossy.
Block Wall(const glm::ivec3& p, float mossChance = 0.18f) {
    return Block(Noise::Random01(p.x, p.y, p.z, kSeed ^ 0x55u) < mossChance ? BlockType::MOSSY_BRICK : BlockType::STONE_BRICK);
}

const Block kAir{};
const Block kLamp(BlockType::LAMP);
const Block kBrick(BlockType::STONE_BRICK);
const Block kLog(BlockType::LOG);
const Block kChest(BlockType::CHEST); // loot is rolled by the server when first opened (see LootFor)
const Block kFullWater = Block::Water(Block::kMaxWaterVolume);

// Lamps set into the middle of each wall, at `height` above the floor.
void WallLamps(Stamper& s, const Box& in, int height) {
    const glm::ivec3 c = Centre(in);
    const int y = in.min.y + height;
    s.SetIfWall({ in.min.x - 1, y, c.z }, kLamp);
    s.SetIfWall({ in.max.x + 1, y, c.z }, kLamp);
    s.SetIfWall({ c.x, y, in.min.z - 1 }, kLamp);
    s.SetIfWall({ c.x, y, in.max.z + 1 }, kLamp);
}

void Furnish(Stamper& s, const Room& r, const std::vector<Box>& corridors) {
    auto onCorridor = [&](const glm::ivec3& p) {
        return std::any_of(corridors.begin(), corridors.end(), [&](const Box& c) { return c.Contains(p); });
    };
    const Box& in = r.inner;
    const int x0 = in.min.x, x1 = in.max.x, y0 = in.min.y, y1 = in.max.y, z0 = in.min.z, z1 = in.max.z;
    const glm::ivec3 c = Centre(in);
    auto rnd = [&](const glm::ivec3& p, uint32_t salt) { return Noise::Random01(p.x, p.y, p.z, r.seed ^ salt); };

    switch (r.type) {
    case RoomType::Entrance:
        WallLamps(s, in, 2);
        break;

    case RoomType::Hall:
        for (int x = x0 + 2; x <= x1 - 2; x += 4)
            for (int z = z0 + 2; z <= z1 - 2; z += 4) {
                for (int y = y0; y < y1; y++) s.Set({ x, y, z }, kBrick);
                s.Set({ x, y1, z }, kLamp); // lamps crown the pillars
            }
        if (rnd(c, 22) < 0.4f && !onCorridor({ x0 - 1, y0, z0 }) && !onCorridor({ x0, y0, z0 - 1 })) s.Set({ x0, y0, z0 }, kChest);
        break;

    case RoomType::Archive: {
        // Shelves along the walls, with gaps, wherever the wall behind is solid
        // (not a doorway).
        // (Decided from the layout, not by reading the wall: it may be in another chunk.)
        auto shelf = [&](const glm::ivec3& cell, const glm::ivec3& behind, int along) {
            if (along % 5 == 2 || onCorridor(behind)) return;
            for (int y = 0; y < 3; y++) s.Set(cell + glm::ivec3(0, y, 0), kLog);
        };
        for (int x = x0 + 1; x <= x1 - 1; x++) {
            shelf({ x, y0, z0 }, { x, y0, z0 - 1 }, x - x0);
            shelf({ x, y0, z1 }, { x, y0, z1 + 1 }, x - x0);
        }
        for (int z = z0 + 1; z <= z1 - 1; z++) {
            shelf({ x0, y0, z }, { x0 - 1, y0, z }, z - z0);
            shelf({ x1, y0, z }, { x1 + 1, y0, z }, z - z0);
        }
        // A reading table under a hanging lamp.
        s.Set({ c.x, y0, c.z }, kLog);
        s.Set({ c.x + 1, y0, c.z }, kLog);
        s.Set({ c.x - 1, y0, c.z }, kChest); // the archivist's chest, by the table
        s.SetIfWall({ c.x, y1 + 1, c.z }, kLamp);
        break;
    }

    case RoomType::Pool:
        s.Fill(PoolBasin(r), [&](const glm::ivec3& p) { s.Set(p, kFullWater); });
        WallLamps(s, in, 2);
        break;

    case RoomType::Garden: {
        // Moss everywhere, a grass floor with bushes and tall grass, lit from above.
        s.Fill(Expand(in, 1), [&](const glm::ivec3& p) {
            if (s.IsWall(p)) s.At(p) = Block(BlockType::MOSSY_BRICK);
        });
        for (int x = x0; x <= x1; x++)
            for (int z = z0; z <= z1; z++) {
                // The floor may be in the chunk below the plants: decide from position.
                const glm::ivec3 floor(x, y0 - 1, z);
                s.Set(floor, Block(BlockType::GRASS));
                const float roll = rnd(floor, 1);
                if (roll < 0.12f && !onCorridor({ x, y0, z })) { // bushes keep paths clear
                    s.Set({ x, y0, z }, Block(BlockType::LEAVES));
                    if (rnd(floor, 2) < 0.4f) s.Set({ x, y0 + 1, z }, Block(BlockType::LEAVES));
                }
                else if (roll < 0.45f) {
                    const float kind = rnd(floor, 3);
                    s.Set({ x, y0, z }, Block::Plant(kind < 0.85f ? BlockType::TALL_GRASS : kind < 0.93f ? BlockType::FLOWER_RED : BlockType::FLOWER_YELLOW, 0));
                }
            }
        for (int x = x0 + 2; x <= x1 - 2; x += 4)
            for (int z = z0 + 2; z <= z1 - 2; z += 4) s.SetIfWall({ x, y1 + 1, z }, kLamp);
        break;
    }

    case RoomType::Crypt:
        // Two rows of tombs, low lamps.
        for (int x = x0 + 1; x <= x1 - 1; x += 3) {
            if (z1 - z0 >= 6) {
                s.Set({ x, y0, z0 + 1 }, Block(BlockType::STONE));
                s.Set({ x, y0, z0 + 2 }, Block(BlockType::STONE));
                s.Set({ x, y0, z1 - 1 }, Block(BlockType::STONE));
                s.Set({ x, y0, z1 - 2 }, Block(BlockType::STONE));
            }
        }
        if (!onCorridor({ x1 + 1, y0, c.z })) s.Set({ x1, y0, c.z }, kChest); // grave goods, at the end of the aisle
        WallLamps(s, in, 1);
        break;

    case RoomType::Vault:
        // Clean bricks, lamps set in the floor around a lit pedestal... which is ringed
        // by crumbling floor over a pit of spikes.
        s.Fill(Expand(in, 1), [&](const glm::ivec3& p) {
            if (s.IsWall(p)) s.At(p) = kBrick;
        });
        for (int x = c.x - 2; x <= c.x + 2; x++)
            for (int z = c.z - 2; z <= c.z + 2; z++)
                if (((x + z) & 1) == 0 && glm::ivec3(x, y0, z) != glm::ivec3(c.x, y0, c.z)) s.SetIfWall({ x, y0 - 1, z }, kLamp);
        for (int x = c.x - 1; x <= c.x + 1; x++)
            for (int z = c.z - 1; z <= c.z + 1; z++) {
                if (x == c.x && z == c.z) {
                    for (int y = y0 - 3; y <= y0 - 1; y++) s.Set({ x, y, z }, kBrick); // the pedestal's footing
                    continue;
                }
                s.Set({ x, y0 - 1, z }, Block(BlockType::CRUMBLING_BRICK));
                s.Set({ x, y0 - 3, z }, Block::Plant(BlockType::SPIKES, 0));
            }
        s.Set({ c.x, y0, c.z }, kBrick);
        s.Set({ c.x, y0 + 1, c.z }, kChest); // the prize, on the pedestal (lit by the floor lamps)
        break;

    case RoomType::FloodChamber: {
        // A reservoir over the ceiling behind sluice gates, and a pressure plate in the
        // middle of the floor. The plate sets a latch gate in the ceiling, which opens
        // the gates and keeps them open; a lever hidden in a wall resets it (and, left
        // on, disarms the trap). Wired on channels: FloodChamberSignals.
        s.Fill(Reservoir(r), [&](const glm::ivec3& p) { s.Set(p, kFullWater); });
        for (int x = c.x - 1; x <= c.x + 1; x++)
            for (int z = c.z - 1; z <= c.z + 1; z++) s.Set({ x, y1 + 1, z }, Block(BlockType::SLUICE));
        s.Set({ c.x, y0, c.z }, Block::PressurePlate());
        s.Set(FloodLatch(r), Block(BlockType::LOGIC_GATE));
        s.SetIfWall(FloodLever(r), Block(BlockType::LEVER));
        s.Fill(Expand(in, 1), [&](const glm::ivec3& p) {
            if (s.IsWall(p) && rnd(p, 6) < 0.5f) s.At(p) = Block(BlockType::MOSSY_BRICK); // damp
        });
        WallLamps(s, in, 2);
        break;
    }

    case RoomType::Collapsed:
        // Heaps of rubble away from the walls (so doorways stay clear).
        for (int x = x0 + 2; x <= x1 - 2; x++)
            for (int z = z0 + 2; z <= z1 - 2; z++) {
                const float n = Noise::Perlin3(x / 4.0f, static_cast<float>(y0), z / 4.0f, r.seed);
                const int height = std::clamp(static_cast<int>((n + 0.25f) * 5.0f), 0, 3);
                for (int y = 0; y < height; y++) {
                    const float roll = rnd({ x, y0 + y, z }, 4);
                    s.Set({ x, y0 + y, z }, Block(roll < 0.55f ? BlockType::STONE : roll < 0.8f ? BlockType::DIRT : BlockType::SAND));
                }
            }
        s.SetIfWall({ x0 - 1, y0 + 2, c.z }, kLamp); // one lamp still burning
        if (rnd(c, 23) < 0.35f && !onCorridor({ x1 + 1, y0, z1 }) && !onCorridor({ x1, y0, z1 + 1 })) s.Set({ x1, y0, z1 }, kChest); // half buried
        break;

    case RoomType::Cistern: {
        // Water three deep in the pit (one block below the ledge, so you can climb
        // out), and pillars rising from the bottom at its corners.
        const Box pit = CisternPit(r);
        s.Fill({ pit.min, { pit.max.x, pit.min.y + 2, pit.max.z } }, [&](const glm::ivec3& p) { s.Set(p, kFullWater); });
        for (const int x : { pit.min.x, pit.max.x })
            for (const int z : { pit.min.z, pit.max.z })
                for (int y = pit.min.y; y <= y1; y++) s.Set({ x, y, z }, kBrick);
        WallLamps(s, in, 3);
        break;
    }

    case RoomType::PumpRoom: {
        // Lower basin (water) with the pump under it; a pipe runs beneath the floor, up
        // the room and along the ceiling, ending over a raised tub at the far end.
        const int zm = r.lane, w = PumpLength(r);
        s.Fill(PumpBasin(r), [&](const glm::ivec3& p) { s.Set(p, kFullWater); });
        s.Set({ PumpX(r, 2), y0 - 3, zm }, Block(BlockType::PUMP));
        const Block pipe(BlockType::PIPE);
        s.Set({ PumpX(r, 3), y0 - 3, zm }, pipe);
        for (int y = y0 - 3; y <= y1; y++) s.Set({ PumpX(r, 4), y, zm }, pipe);
        for (int k = 5; k <= w - 3; k++) s.Set({ PumpX(r, k), y1, zm }, pipe);
        // The tub: inner offsets w-4..w-2 by zm-1..zm+1, two deep, on a brick plinth.
        s.Fill(PumpTub(r), [&](const glm::ivec3& p) {
            const int k = r.flip ? x1 - p.x : p.x - x0;
            const bool inner = k >= w - 4 && k <= w - 2 && std::abs(p.z - zm) <= 1;
            s.Set(p, inner && p.y > y0 + 1 ? kAir : kBrick);
        });
        WallLamps(s, in, 2);
        break;
    }

    case RoomType::Shrine:
        s.Fill(Expand(in, 1), [&](const glm::ivec3& p) {
            if (s.IsWall(p) && rnd(p, 5) < 0.35f) s.At(p) = Block(BlockType::MOSSY_BRICK);
        });
        s.Set({ c.x, y0, c.z }, kBrick);
        s.Set({ c.x, y0 + 1, c.z }, kBrick);
        s.Set({ c.x, y0 + 2, c.z }, kLamp);
        if (rnd(c, 21) < 0.6f) s.Set({ c.x, y0, c.z - 1 }, kChest); // offerings, behind the plinth
        // Pews facing the plinth, with an aisle down the middle.
        for (int z = c.z + 2; z <= z1 - 1; z += 2)
            for (int x = x0 + 1; x <= x1 - 1; x++)
                if (std::abs(x - c.x) > 1) s.Set({ x, y0, z }, kLog);
        break;

    case RoomType::Count:
        break;
    }
}

} // namespace

const char* RoomName(RoomType type) {
    switch (type) {
    case RoomType::Entrance: return "entrance";
    case RoomType::Hall: return "pillared hall";
    case RoomType::Archive: return "archive";
    case RoomType::Pool: return "pool";
    case RoomType::Garden: return "overgrown garden";
    case RoomType::Crypt: return "crypt";
    case RoomType::Vault: return "vault";
    case RoomType::Collapsed: return "collapsed room";
    case RoomType::Cistern: return "cistern";
    case RoomType::PumpRoom: return "pump room";
    case RoomType::Shrine: return "shrine";
    case RoomType::FloodChamber: return "flood chamber";
    default: return "room";
    }
}

Layout LayoutFor(const TerrainGenerator& terrain, int regionX, int regionZ) {
    Layout layout;
    if (Noise::Random01(regionX, 7, regionZ, kSeed) >= kDungeonChance) return layout;
    Rng rng{ Noise::Hash(regionX, 0, regionZ, kSeed) | 1u };

    layout.floorY = -52 + rng.Range(0, 9); // rooms' tops stay below -36, under any sea floor
    const int baseX = regionX * kRegionSize, baseZ = regionZ * kRegionSize;
    const int target = rng.Range(7, 12);

    // Rooms, scattered without overlapping (with a gap for walls and corridors).
    for (int attempt = 0; attempt < 400 && static_cast<int>(layout.rooms.size()) < target; attempt++) {
        const int w = rng.Range(7, 15), d = rng.Range(7, 15);
        const int x0 = baseX + 8 + rng.Range(0, kRegionSize - 16 - w);
        const int z0 = baseZ + 8 + rng.Range(0, kRegionSize - 16 - d);
        Room room;
        room.inner = { { x0, layout.floorY, z0 }, { x0 + w - 1, layout.floorY + 5, z0 + d - 1 } };
        bool clear = true;
        for (const auto& other : layout.rooms)
            if (Overlaps(Expand(room.inner, 4), other.inner)) clear = false;
        if (!clear) continue;
        room.seed = rng.Next();
        layout.rooms.push_back(room);
    }
    if (layout.rooms.size() < 3) return layout; // too crowded to bother

    // Types: the first room is the entrance; the rest work through a shuffled list of
    // all the others (so a dungeon repeats a type only once it has used them all),
    // taking the first that fits each room.
    std::vector<RoomType> pool;
    auto refill = [&] {
        for (int t = 1; t < static_cast<int>(RoomType::Count); t++) pool.push_back(static_cast<RoomType>(t));
        for (size_t i = pool.size() - 1; i > 0; i--) std::swap(pool[i], pool[rng.Next() % (i + 1)]);
    };
    for (size_t i = 0; i < layout.rooms.size(); i++) {
        Room& room = layout.rooms[i];
        const int w = room.inner.max.x - room.inner.min.x + 1, d = room.inner.max.z - room.inner.min.z + 1;
        if (i == 0) room.type = RoomType::Entrance;
        else {
            if (pool.empty()) refill();
            auto it = std::find_if(pool.begin(), pool.end(), [&](RoomType t) { return Fits(t, w, d); });
            if (it == pool.end()) room.type = RoomType::Vault;
            else {
                room.type = *it;
                pool.erase(it);
            }
        }
        room.inner.max.y = layout.floorY + HeightFor(room.type) - 1;
    }

    // Corridors: a spanning tree over the rooms (nearest first, Prim's), plus a few
    // extra links so there are loops. Each link is an L of two 3-wide segments.
    const size_t n = layout.rooms.size();
    std::vector<std::pair<size_t, size_t>> links;
    std::vector<bool> joined(n, false);
    joined[0] = true;
    auto dist2 = [&](size_t a, size_t b) {
        const glm::ivec3 d = Centre(layout.rooms[a].inner) - Centre(layout.rooms[b].inner);
        return d.x * d.x + d.z * d.z;
    };
    for (size_t step = 1; step < n; step++) {
        size_t bestA = 0, bestB = 0;
        int best = std::numeric_limits<int>::max();
        for (size_t a = 0; a < n; a++)
            if (joined[a])
                for (size_t b = 0; b < n; b++)
                    if (!joined[b] && dist2(a, b) < best) best = dist2(a, b), bestA = a, bestB = b;
        joined[bestB] = true;
        links.emplace_back(bestA, bestB);
    }
    for (size_t a = 0; a < n; a++) {
        if (rng.Unit() > 0.25f) continue;
        size_t near = a;
        int best = std::numeric_limits<int>::max();
        for (size_t b = 0; b < n; b++) {
            const bool linked = std::any_of(links.begin(), links.end(), [&](auto& l) { return (l.first == a && l.second == b) || (l.first == b && l.second == a); });
            if (b != a && !linked && dist2(a, b) < best) best = dist2(a, b), near = b;
        }
        if (near != a) links.emplace_back(a, near);
    }
    const int fy = layout.floorY;
    for (const auto& [a, b] : links) {
        const glm::ivec3 ca = Centre(layout.rooms[a].inner), cb = Centre(layout.rooms[b].inner);
        layout.corridors.push_back({ { std::min(ca.x, cb.x) - 1, fy, ca.z - 1 }, { std::max(ca.x, cb.x) + 1, fy + 2, ca.z + 1 } });
        layout.corridors.push_back({ { cb.x - 1, fy, std::min(ca.z, cb.z) - 1 }, { cb.x + 1, fy + 2, std::max(ca.z, cb.z) + 1 } });
    }

    // Pump rooms: find a side and a lane where the raised tub blocks no corridor (one
    // may run straight through the room); with none, make it a pool or a vault.
    for (auto& room : layout.rooms) {
        if (room.type != RoomType::PumpRoom) continue;
        bool placed = false;
        const int zc = Centre(room.inner).z;
        for (int flip = 0; flip < 2 && !placed; flip++)
            for (int k = 0; k <= room.inner.max.z - room.inner.min.z && !placed; k++) {
                const int lane = zc + ((k & 1) ? (k + 1) / 2 : -(k / 2)); // centre outward
                if (lane - 2 < room.inner.min.z || lane + 2 > room.inner.max.z) continue;
                room.flip = flip == 1;
                room.lane = lane;
                const Box tub = PumpTub(room);
                placed = std::none_of(layout.corridors.begin(), layout.corridors.end(), [&](const Box& c) { return Overlaps(c, tub); });
            }
        if (!placed) {
            const int w = room.inner.max.x - room.inner.min.x + 1, d = room.inner.max.z - room.inner.min.z + 1;
            room.type = Fits(RoomType::Pool, w, d) ? RoomType::Pool : RoomType::Vault;
            room.inner.max.y = layout.floorY + HeightFor(room.type) - 1;
        }
    }

    // Spike pits across some long corridors, away from rooms and crossings: hidden
    // under crumbling bricks (three long), or open (two long, so they can be jumped).
    for (size_t i = 0; i < layout.corridors.size(); i++) {
        const Box& c = layout.corridors[i];
        const bool alongX = c.max.x - c.min.x > c.max.z - c.min.z;
        const int lo = alongX ? c.min.x : c.min.z, hi = alongX ? c.max.x : c.max.z;
        if (hi - lo + 1 < 16 || rng.Unit() > 0.4f) continue;
        const bool covered = rng.Unit() < 0.6f;
        const int length = covered ? 3 : 2;
        const int start = rng.Range(lo + 6, hi - 6 - length + 1);
        Box open = c;
        open.min.y = fy - 3;
        open.max.y = fy - 1;
        (alongX ? open.min.x : open.min.z) = start;
        (alongX ? open.max.x : open.max.z) = start + length - 1;
        const Box shellBox = Expand(open, 1);
        bool clear = true;
        for (const auto& room : layout.rooms) {
            Box annex;
            if (Overlaps(shellBox, Expand(room.inner, 1)) || (Annex(room, annex) && Overlaps(shellBox, Expand(annex, 1)))) clear = false;
        }
        for (size_t j = 0; j < layout.corridors.size(); j++)
            if (j != i && Overlaps(shellBox, Expand(layout.corridors[j], 1))) clear = false;
        for (const auto& pit : layout.pits)
            if (Overlaps(shellBox, Expand(pit.open, 1))) clear = false;
        if (clear) layout.pits.push_back({ open, covered });
    }

    // The entrance reaches the surface, unless it's under the sea or a beach.
    const glm::ivec3 e = Centre(layout.rooms[0].inner);
    const auto column = terrain.ColumnAt(e.x, e.z);
    if (column.surfaceRow > TerrainGenerator::kSeaLevel + 1) {
        layout.hasShaft = true;
        layout.shaft = { e.x, e.z };
        layout.shaftTop = column.FirstOpenRow();
    }

    // Bounds: shells, sunken parts, and the shaft up to its clearance.
    layout.bounds = Expand(layout.rooms[0].inner, 1);
    for (const auto& room : layout.rooms) {
        layout.bounds = Union(layout.bounds, Expand(room.inner, 1));
        Box sunk;
        if (Annex(room, sunk)) layout.bounds = Union(layout.bounds, Expand(sunk, 1));
    }
    for (const auto& c : layout.corridors) layout.bounds = Union(layout.bounds, Expand(c, 1));
    for (const auto& pit : layout.pits) layout.bounds = Union(layout.bounds, Expand(pit.open, 1));
    if (layout.hasShaft)
        layout.bounds = Union(layout.bounds, { { e.x - 2, fy, e.z - 2 }, { e.x + 2, layout.shaftTop + kShaftClearance, e.z + 2 } });
    layout.exists = true;
    return layout;
}

void Stamp(const TerrainGenerator& terrain, const glm::ivec3& chunkCoords, std::array<Block, Chunk::VOLUME>& blocks) {
    // Dungeons lie between about -60 and the shaft tops: skip chunks far from that
    // before working out any layout.
    const int bottom = chunkCoords.y * S, top = bottom + S - 1;
    if (top < -64 || bottom > 160) return; // (shafts may climb a mountain)

    // A region is a whole number of chunks, so a chunk is in exactly one.
    const Layout layout = LayoutFor(terrain, FloorDiv(chunkCoords.x * S, kRegionSize), FloorDiv(chunkCoords.z * S, kRegionSize));
    if (!layout.exists) return;
    Stamper s{ chunkCoords * S, blocks, { chunkCoords * S, chunkCoords * S + (S - 1) } };
    if (!Overlaps(layout.bounds, s.chunk)) return;

    const int fy = layout.floorY;
    const Room& entrance = layout.rooms[0];
    const Box shaftShell{ { layout.shaft.x - 2, entrance.inner.max.y + 1, layout.shaft.y - 2 }, { layout.shaft.x + 2, layout.shaftTop - 1, layout.shaft.y + 2 } };
    const Box shaftOpen{ { layout.shaft.x - 1, fy, layout.shaft.y - 1 }, { layout.shaft.x + 1, layout.shaftTop + kShaftClearance, layout.shaft.y + 1 } };

    // 1. Shells.
    auto shell = [&](const glm::ivec3& p) { s.At(p) = Wall(p); };
    for (const auto& room : layout.rooms) {
        s.Fill(Expand(room.inner, 1), shell);
        Box sunk;
        if (Annex(room, sunk)) s.Fill(Expand(sunk, 1), shell);
    }
    for (const auto& c : layout.corridors) s.Fill(Expand(c, 1), shell);
    for (const auto& pit : layout.pits) s.Fill(Expand(pit.open, 1), shell);
    if (layout.hasShaft) s.Fill(shaftShell, shell);

    // 2. Interiors.
    auto open = [&](const glm::ivec3& p) { s.At(p) = kAir; };
    for (const auto& room : layout.rooms) {
        s.Fill(room.inner, open);
        Box sunk;
        if (Annex(room, sunk)) s.Fill(sunk, open);
    }
    for (const auto& c : layout.corridors) s.Fill(c, open);
    for (const auto& pit : layout.pits) s.Fill(pit.open, open);
    if (layout.hasShaft) s.Fill(shaftOpen, open);

    // 3. Furnishings.
    for (const auto& room : layout.rooms) Furnish(s, room, layout.corridors);
    for (const auto& pit : layout.pits)
        s.Fill(pit.open, [&](const glm::ivec3& p) {
            if (p.y == pit.open.min.y) s.Set(p, Block::Plant(BlockType::SPIKES, 0));
            else if (p.y == pit.open.max.y && pit.covered) s.Set(p, Block(BlockType::CRUMBLING_BRICK));
        });
    for (const auto& c : layout.corridors) {
        // Lamps in the side walls every 8 blocks, alternating sides.
        const bool alongX = c.max.x - c.min.x > c.max.z - c.min.z;
        if (alongX) {
            for (int x = c.min.x; x <= c.max.x; x++)
                if (x % 8 == 0) s.SetIfWall({ x, fy + 2, (x % 16 == 0) ? c.min.z - 1 : c.max.z + 1 }, kLamp);
        }
        else {
            for (int z = c.min.z; z <= c.max.z; z++)
                if (z % 8 == 0) s.SetIfWall({ (z % 16 == 0) ? c.min.x - 1 : c.max.x + 1, fy + 2, z }, kLamp);
        }
    }
    if (layout.hasShaft) {
        // A spiral staircase round a central pillar, from the entrance's floor up to
        // the surface: one step per block, round the eight cells about the pillar.
        const glm::ivec2 c = layout.shaft;
        const glm::ivec2 around[8] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 } };
        for (int y = fy; y <= layout.shaftTop; y++) {
            s.Set({ c.x, y, c.y }, kBrick);
            const glm::ivec2 step = c + around[(y - fy) % 8];
            s.Set({ step.x, y, step.y }, kBrick);
        }
        // A ring of bricks on the ground round the opening (on each column's own
        // ground, so it never hangs over air).
        for (int dx = -2; dx <= 2; dx++)
            for (int dz = -2; dz <= 2; dz++) {
                if (std::max(std::abs(dx), std::abs(dz)) != 2) continue;
                const int ground = terrain.ColumnAt(c.x + dx, c.y + dz).FirstOpenRow();
                s.Set({ c.x + dx, ground, c.y + dz }, kBrick);
            }
    }
}

std::vector<std::pair<glm::ivec3, Signals::Config>> FloodChamberSignals(const TerrainGenerator& terrain, const glm::ivec3& pos) {
    std::vector<std::pair<glm::ivec3, Signals::Config>> out;
    const Layout layout = LayoutFor(terrain, FloorDiv(pos.x, kRegionSize), FloorDiv(pos.z, kRegionSize));
    if (!layout.exists) return out;
    for (const auto& r : layout.rooms) {
        if (r.type != RoomType::FloodChamber || !Expand(r.inner, 1).Contains(pos)) continue;
        const glm::ivec3 c = Centre(r.inner);
        // Channels from the room's seed, high up out of the way of players' own: the
        // plate's (setting the latch), the lever's (resetting it), and the latch's (to
        // the sluice gates).
        auto channel = [&](uint32_t n) { return static_cast<uint8_t>(200 + (r.seed + n) % 56); };
        Signals::Config plate;
        plate.kind = Signals::Kind::Plate;
        plate.out = channel(0);
        out.emplace_back(glm::ivec3(c.x, r.inner.min.y, c.z), plate);
        Signals::Config lever;
        lever.kind = Signals::Kind::Lever;
        lever.out = channel(1);
        out.emplace_back(FloodLever(r), lever);
        Signals::Config latch;
        latch.kind = Signals::Kind::Gate;
        latch.mode = Signals::Mode::Latch;
        latch.a = channel(0);
        latch.b = channel(1);
        latch.out = channel(2);
        out.emplace_back(FloodLatch(r), latch);
        Signals::Config gate;
        gate.kind = Signals::Kind::Sluice;
        gate.a = channel(2);
        for (int x = c.x - 1; x <= c.x + 1; x++)
            for (int z = c.z - 1; z <= c.z + 1; z++) out.emplace_back(glm::ivec3(x, r.inner.max.y + 1, z), gate);
    }
    return out;
}

std::vector<std::pair<BlockType, uint32_t>> LootFor(const TerrainGenerator& terrain, const glm::ivec3& pos) {
    std::vector<std::pair<BlockType, uint32_t>> loot;
    const Layout layout = LayoutFor(terrain, FloorDiv(pos.x, kRegionSize), FloorDiv(pos.z, kRegionSize));
    if (!layout.exists) return loot;
    const Room* room = nullptr;
    for (const auto& r : layout.rooms)
        if (Expand(r.inner, 1).Contains(pos)) room = &r;
    if (!room) return loot;
    const bool rich = room->type == RoomType::Vault;
    struct Entry { BlockType type; int least, most; };
    static const Entry kCommon[] = {
        { BlockType::COOKED_VENISON, 1, 3 }, { BlockType::RAW_VENISON, 1, 2 }, { BlockType::HIDE, 1, 3 }, { BlockType::LAMP, 1, 3 },
        { BlockType::PLANKS, 4, 10 }, { BlockType::GLASS, 2, 5 }, { BlockType::STONE_BRICK, 4, 12 }, { BlockType::MOSSY_BRICK, 2, 6 },
        { BlockType::PIPE, 2, 6 }, { BlockType::LOG, 2, 5 },
    };
    static const Entry kRich[] = {
        { BlockType::COOKED_VENISON, 3, 6 }, { BlockType::HIDE, 3, 8 }, { BlockType::LAMP, 3, 8 }, { BlockType::PUMP, 1, 2 },
        { BlockType::SLUICE, 1, 3 }, { BlockType::GLASS, 6, 12 }, { BlockType::PIPE, 6, 16 }, { BlockType::SPIKES, 2, 4 },
        { BlockType::CHEST, 1, 2 }, { BlockType::BED, 1, 1 },
    };
    const Entry* table = rich ? kRich : kCommon;
    const int tableSize = rich ? static_cast<int>(std::size(kRich)) : static_cast<int>(std::size(kCommon));
    const uint32_t seed = room->seed ^ 0x100Du;
    const int picks = (rich ? 5 : 3) + static_cast<int>(Noise::Random01(pos.x, pos.y, pos.z, seed) * 3.0f); // 3-5, or 5-7
    for (int i = 0; i < picks; i++) {
        const Entry& e = table[static_cast<int>(Noise::Random01(pos.x, pos.y + i, pos.z, seed ^ 0x51u) * tableSize) % tableSize];
        const int count = e.least + static_cast<int>(Noise::Random01(pos.x + i, pos.y, pos.z, seed ^ 0x77u) * (e.most - e.least + 1));
        auto it = std::find_if(loot.begin(), loot.end(), [&](const auto& l) { return l.first == e.type; });
        const uint32_t units = static_cast<uint32_t>(std::min(count, e.most)) * Block::kUnitsPerBlock;
        if (it != loot.end()) it->second += units;
        else loot.emplace_back(e.type, units);
    }
    return loot;
}

} // namespace Dungeons
