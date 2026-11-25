// Dungeon generation. Lays out the dungeons of a block of regions, generates their
// chunks with the real terrain generator, and checks that
//  - most regions have one, and between them they use every room type,
//  - layouts and chunks come out the same every time,
//  - every room has an open interior and solid corners,
//  - every room can be reached from the entrance through open space (corridors work
//    across chunk borders), and the entrance shaft (where there is one) reaches the
//    surface,
//  - water sits in sealed basins: nothing under or beside it is open,
//  - rooms are well below the ground,
//  - pump rooms have a pump under water with a pipe beside it.
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "world/Coords.h"
#include "world/Dungeons.h"
#include "world/TerrainGenerator.h"

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

// Generated chunks on demand.
struct World {
    TerrainGenerator terrain;
    std::unordered_map<glm::ivec3, std::unique_ptr<TerrainGenerator::Blocks>, Coords::IVec3Hash> chunks;
    double generateSeconds = 0.0;
    size_t generated = 0;

    const Block& Get(const glm::ivec3& p) {
        const glm::ivec3 c = Coords::ChunkOf(p);
        auto& chunk = chunks[c];
        if (!chunk) {
            chunk = std::make_unique<TerrainGenerator::Blocks>();
            TerrainGenerator::ColumnHeights heights;
            const auto t0 = std::chrono::steady_clock::now();
            terrain.ComputeColumnHeights(c.x, c.z, heights);
            terrain.GenerateChunk(c, heights, *chunk);
            generateSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            generated++;
        }
        const glm::ivec3 l = Coords::LocalOf(p);
        return (*chunk)[Chunk::Index(l.x, l.y, l.z)];
    }
};

bool IsBrick(const Block& b) { return b.type == BlockType::STONE_BRICK || b.type == BlockType::MOSSY_BRICK; }
// Somewhere a player can be: not a full block (air, water, plants, slopes).
bool Open(const Block& b) { return !b.IsFullBlock(); }

} // namespace

int main() {
    World world;

    std::cout << "Layouts" << std::endl;
    std::vector<std::pair<glm::ivec2, Dungeons::Layout>> dungeons;
    std::set<Dungeons::RoomType> types;
    int regions = 0;
    for (int rx = -7; rx <= 7; rx++)
        for (int rz = -7; rz <= 7; rz++) {
            regions++;
            auto layout = Dungeons::LayoutFor(world.terrain, rx, rz);
            if (!layout.exists) continue;
            for (const auto& r : layout.rooms) types.insert(r.type);
            dungeons.emplace_back(glm::ivec2(rx, rz), std::move(layout));
        }
    // About three regions in ten: rare enough to be a find.
    Check(dungeons.size() * 100 >= static_cast<size_t>(regions) * 15 && dungeons.size() * 100 <= static_cast<size_t>(regions) * 45,
          std::to_string(dungeons.size()) + " of " + std::to_string(regions) + " regions have a dungeon (about 30% wanted)");
    Check(types.size() == static_cast<size_t>(Dungeons::RoomType::Count), std::to_string(types.size()) + " room types appear");
    {
        // Over a wider sweep (layouts only, nothing generated): no entrance comes up in
        // or beside a lake or a river, which would pour down it; and a lever vault's
        // chest pit is within the dungeon's bounds (or a chunk holding only it is skipped).
        bool dry = true, pitsInside = true;
        int shafts = 0;
        for (int rx = -30; rx <= 30; rx++)
            for (int rz = -30; rz <= 30; rz++) {
                const auto layout = Dungeons::LayoutFor(world.terrain, rx, rz);
                if (!layout.exists) continue;
                for (const auto& r : layout.rooms)
                    if (r.type == Dungeons::RoomType::LeverVault) {
                        const glm::ivec3 c = (r.inner.min + r.inner.max) / 2;
                        pitsInside &= layout.bounds.Contains({ c.x, r.inner.min.y - 3, c.z });
                    }
                if (!layout.hasShaft) continue;
                shafts++;
                const glm::ivec2 e = layout.shaft;
                if (world.terrain.InRiver(e.x, e.y, TerrainGenerator::kRiverBanks)) dry = false;
                for (int dx = -3; dx <= 3; dx++)
                    for (int dz = -3; dz <= 3; dz++)
                        if (world.terrain.LakeLevelAt(e.x + dx, e.y + dz) != INT_MIN) dry = false;
            }
        Check(dry && shafts > 100, "no entrance shaft comes up in a lake or a river (" + std::to_string(shafts) + " looked at)");
        Check(pitsInside, "a lever vault's chest pit is within its dungeon's bounds");
    }
    {
        std::map<std::string, int> all;
        for (const auto& [region, layout] : dungeons)
            for (const auto& r : layout.rooms) all[Dungeons::RoomName(r.type)]++;
        std::string mix;
        for (const auto& [name, count] : all) mix += (mix.empty() ? "" : ", ") + name + " " + std::to_string(count);
        std::cout << "        across all of them: " << mix << std::endl;
    }
    {
        const auto again = Dungeons::LayoutFor(world.terrain, dungeons[0].first.x, dungeons[0].first.y);
        bool same = again.rooms.size() == dungeons[0].second.rooms.size() && again.corridors.size() == dungeons[0].second.corridors.size();
        for (size_t i = 0; same && i < again.rooms.size(); i++)
            same = again.rooms[i].inner.min == dungeons[0].second.rooms[i].inner.min && again.rooms[i].type == dungeons[0].second.rooms[i].type;
        Check(same, "a region's layout comes out the same every time");
    }

    std::cout << "Generated dungeons" << std::endl;
    bool interiors = true, corners = true, connected = true, sealed = true, deep = true, shafts = true, pumps = true;
    bool gardens = true, archives = true; // furnished all over, whatever the chunk borders
    bool pitsOk = true, vaultsOk = true, vaultChestsOk = true, floodsOk = true, floodWired = true;
    int coveredPits = 0, openPits = 0, vaults = 0, floods = 0, floodLevers = 0, leverVaults = 0;
    bool leverVaultsBuilt = true, leverVaultsWired = true, leverVaultsHinted = true;
    int checked = 0, withShaft = 0, pumpRooms = 0, rooms = 0;
    std::map<std::string, int> typeCounts;
    for (const auto& entry : dungeons) {
        const Dungeons::Layout& layout = entry.second; // (a plain reference: lambdas below capture it)
        if (checked == 16) break;
        checked++;
        const Dungeons::Box& bounds = layout.bounds;

        for (const auto& r : layout.rooms) {
            rooms++;
            typeCounts[Dungeons::RoomName(r.type)]++;
            const glm::ivec3 lo = r.inner.min, hi = r.inner.max;
            if (!world.Get({ lo.x + 1, hi.y, lo.z + 1 }).IsAir()) interiors = false;
            // Corners aren't doorways, but another corridor may pass through one.
            auto inCorridor = [&](const glm::ivec3& p) {
                return std::any_of(layout.corridors.begin(), layout.corridors.end(), [&](const Dungeons::Box& c) { return c.Contains(p); });
            };
            for (const glm::ivec3 corner : { glm::ivec3(lo.x - 1, lo.y, lo.z - 1), glm::ivec3(hi.x + 1, hi.y + 1, hi.z + 1) })
                if (!inCorridor(corner) && !IsBrick(world.Get(corner))) corners = false;
            // Furnishings everywhere in the room, not just where they share a chunk with
            // the floor or wall they stand against.
            if (r.type == Dungeons::RoomType::Garden) {
                int found = 0, cells = 0;
                for (int x = lo.x; x <= hi.x; x++)
                    for (int z = lo.z; z <= hi.z; z++, cells++) {
                        const Block& b = world.Get({ x, lo.y, z });
                        found += b.IsPlant() || b.type == BlockType::LEAVES;
                    }
                if (found * 4 < cells) gardens = false; // about 45% of the floor
            }
            if (r.type == Dungeons::RoomType::Archive) {
                // Exactly where the layout says: every wall cell (not a corner) that isn't
                // a gap or a doorway has a shelf.
                auto doorway = [&](const glm::ivec3& p) {
                    return std::any_of(layout.corridors.begin(), layout.corridors.end(), [&](const Dungeons::Box& c) { return c.Contains(p); });
                };
                auto expect = [&](const glm::ivec3& cell, const glm::ivec3& behind, int along) {
                    const bool want = along % 5 != 2 && !doorway(behind);
                    if (want != (world.Get(cell).type == BlockType::LOG)) archives = false;
                };
                for (int x = lo.x + 1; x <= hi.x - 1; x++) {
                    expect({ x, lo.y, lo.z }, { x, lo.y, lo.z - 1 }, x - lo.x);
                    expect({ x, lo.y, hi.z }, { x, lo.y, hi.z + 1 }, x - lo.x);
                }
                for (int z = lo.z + 1; z <= hi.z - 1; z++) {
                    expect({ lo.x, lo.y, z }, { lo.x - 1, lo.y, z }, z - lo.z);
                    expect({ hi.x, lo.y, z }, { hi.x + 1, lo.y, z }, z - lo.z);
                }
            }
            // Traps in rooms.
            const glm::ivec3 c = (lo + hi) / 2;
            if (r.type == Dungeons::RoomType::Vault) {
                vaults++;
                // The prize on the pedestal, with rich loot in it.
                const glm::ivec3 prize(c.x, lo.y + 1, c.z);
                if (world.Get(prize).type != BlockType::CHEST) vaultChestsOk = false;
                uint32_t worth = 0;
                for (const auto& [type, units] : Dungeons::LootFor(world.terrain, prize)) worth += units;
                if (worth < 8 * Block::kUnitsPerBlock) vaultChestsOk = false;
                for (int x = c.x - 1; x <= c.x + 1; x++)
                    for (int z = c.z - 1; z <= c.z + 1; z++) {
                        if (x == c.x && z == c.z) continue;
                        if (world.Get({ x, lo.y - 1, z }).type != BlockType::CRUMBLING_BRICK || world.Get({ x, lo.y - 3, z }).type != BlockType::SPIKES ||
                            !world.Get({ x, lo.y - 2, z }).IsAir())
                            vaultsOk = false;
                    }
            }
            if (r.type == Dungeons::RoomType::FloodChamber) {
                floods++;
                if (world.Get({ c.x, lo.y, c.z }).type != BlockType::PRESSURE_PLATE || world.Get({ c.x, hi.y + 1, c.z }).type != BlockType::SLUICE ||
                    !world.Get({ c.x, hi.y + 2, c.z }).IsFullWater() || !world.Get({ lo.x + 1, hi.y + 4, lo.z + 1 }).IsFullWater())
                    floodsOk = false;
                // Wired: the plate sets a latch gate (really there, in the ceiling), a
                // lever resets it, and each of the 9 sluice gates (where they really
                // are) listens to the latch.
                const auto wiring = Dungeons::FloodChamberSignals(world.terrain, { c.x, lo.y, c.z });
                int plates = 0, latches = 0, gates = 0;
                uint8_t plateChannel = 0, leverChannel = 0, latchChannel = 0;
                for (const auto& [p, config] : wiring) {
                    if (config.kind == Signals::Kind::Plate && p == glm::ivec3(c.x, lo.y, c.z)) plates++, plateChannel = config.out;
                    if (config.kind == Signals::Kind::Lever) leverChannel = config.out;
                }
                for (const auto& [p, config] : wiring)
                    if (config.kind == Signals::Kind::Gate && config.mode == Signals::Mode::Latch && config.a == plateChannel && config.b == leverChannel &&
                        p.y == hi.y + 1 && world.Get(p).type == BlockType::LOGIC_GATE)
                        latches++, latchChannel = config.out;
                for (const auto& [p, config] : wiring)
                    if (config.kind == Signals::Kind::Sluice && config.a == latchChannel && world.Get(p).type == BlockType::SLUICE) gates++;
                const std::set<uint8_t> channels{ plateChannel, leverChannel, latchChannel };
                if (plates != 1 || latches != 1 || gates != 9 || channels.size() != 3 || channels.count(0)) floodWired = false;
                for (const auto& [p, config] : wiring)
                    if (config.kind == Signals::Kind::Lever && world.Get(p).type == BlockType::LEVER) floodLevers++;
            }
            if (r.type == Dungeons::RoomType::LeverVault) {
                leverVaults++;
                const auto v = Dungeons::LeverVaultAt(world.terrain, { c.x, lo.y, c.z });
                bool built = v.levers.size() == 4 && world.Get(v.gate).type == BlockType::SLUICE && world.Get(v.chest).type == BlockType::CHEST;
                for (const auto& l : v.levers) built = built && world.Get(l).type == BlockType::LEVER;
                uint32_t worth = 0;
                for (const auto& [type, units] : Dungeons::LootFor(world.terrain, v.chest)) worth += units;
                if (!built || worth < 8 * Block::kUnitsPerBlock) leverVaultsBuilt = false;
                // The wiring, worked through: the gate opens for the pattern, and for
                // nothing else.
                auto opens = [&](unsigned levers) {
                    std::map<uint8_t, bool> on;
                    for (const auto& [p, config] : v.wiring)
                        for (size_t i = 0; i < v.levers.size(); i++)
                            if (config.kind == Signals::Kind::Lever && p == v.levers[i]) on[config.out] = (levers >> i) & 1u;
                    for (int step = 0; step < 4; step++)
                        for (const auto& [p, config] : v.wiring)
                            if (config.kind == Signals::Kind::Gate) on[config.out] = Signals::Evaluate(config.mode, on[config.a], on[config.b]);
                    for (const auto& [p, config] : v.wiring)
                        if (config.kind == Signals::Kind::Sluice && p == v.gate) return on[config.a];
                    return false;
                };
                bool logicRight = v.pattern != 0 && opens(v.pattern);
                for (unsigned other = 0; other < 16; other++)
                    if (other != v.pattern && opens(other)) logicRight = false;
                if (!logicRight) leverVaultsWired = false;
                // A lamp in the ceiling over each lever that should be on, and none over
                // the rest (every time: a doorway can't take its place).
                for (int i = 0; i < 4; i++) {
                    const bool lit = world.Get({ v.levers[i].x, hi.y + 1, v.levers[i].z }).type == BlockType::LAMP;
                    if (lit != (((v.pattern >> i) & 1u) != 0)) leverVaultsHinted = false;
                }
            }
            // Well below the ground at the room's corners.
            for (const glm::ivec2 c : { glm::ivec2(lo.x, lo.z), glm::ivec2(hi.x, hi.z), glm::ivec2(lo.x, hi.z), glm::ivec2(hi.x, lo.z) })
                if (hi.y + 1 > world.terrain.ColumnAt(c.x, c.y).surfaceRow - 8) deep = false;
        }

        // Corridor spike pits: spikes all along the bottom, a crumbling cover or none.
        for (const auto& pit : layout.pits) {
            (pit.covered ? coveredPits : openPits)++;
            for (int x = pit.open.min.x; x <= pit.open.max.x; x++)
                for (int z = pit.open.min.z; z <= pit.open.max.z; z++) {
                    const BlockType top = world.Get({ x, pit.open.max.y, z }).type;
                    if (world.Get({ x, pit.open.min.y, z }).type != BlockType::SPIKES) pitsOk = false;
                    if (top != (pit.covered ? BlockType::CRUMBLING_BRICK : BlockType::AIR)) pitsOk = false;
                    if (!IsBrick(world.Get({ x, pit.open.min.y - 1, z }))) pitsOk = false; // something to stand the spikes on
                }
        }

        // Flood fill through open space inside the dungeon's bounds, from the entrance.
        const auto& e = layout.rooms[0].inner;
        std::unordered_set<glm::ivec3, Coords::IVec3Hash> seen;
        std::deque<glm::ivec3> queue{ glm::ivec3(e.min.x + 1, e.max.y, e.min.z + 1) };
        seen.insert(queue.front());
        const glm::ivec3 dirs[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        while (!queue.empty()) {
            const glm::ivec3 p = queue.front();
            queue.pop_front();
            for (const auto& d : dirs) {
                const glm::ivec3 q = p + d;
                if (!bounds.Contains(q) || seen.count(q) || !Open(world.Get(q))) continue;
                seen.insert(q);
                queue.push_back(q);
            }
        }
        for (const auto& r : layout.rooms)
            if (!seen.count({ r.inner.min.x + 1, r.inner.max.y, r.inner.min.z + 1 })) connected = false;
        if (layout.hasShaft) {
            withShaft++;
            bool out = false;
            for (const glm::ivec3& p : seen)
                if (p.y > layout.shaftTop + 2) out = true;
            if (!out) shafts = false;
        }

        // Water: nothing open below or beside it (so it can't run anywhere), and pumps.
        for (int x = bounds.min.x; x <= bounds.max.x; x++)
            for (int y = bounds.min.y; y <= std::min(bounds.max.y, layout.floorY + 8); y++)
                for (int z = bounds.min.z; z <= bounds.max.z; z++) {
                    const Block& b = world.Get({ x, y, z });
                    if (b.type == BlockType::PUMP) {
                        pumpRooms++;
                        const bool wet = world.Get({ x, y + 1, z }).IsLiquid();
                        const bool piped = world.Get({ x + 1, y, z }).type == BlockType::FLUID_PIPE || world.Get({ x - 1, y, z }).type == BlockType::FLUID_PIPE;
                        if (!wet || !piped) pumps = false;
                    }
                    if (!b.HasWater()) continue;
                    for (const glm::ivec3 d : { glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1), glm::ivec3(0, -1, 0) }) {
                        const Block& n = world.Get(glm::ivec3(x, y, z) + d);
                        if (!n.HasWater() && !n.IsFullBlock()) sealed = false;
                    }
                }
    }
    Check(interiors, "every room's interior is open (" + std::to_string(rooms) + " rooms in " + std::to_string(checked) + " dungeons)");
    Check(corners, "and its corners are brick");
    Check(connected, "every room can be reached from the entrance, across chunk borders");
    Check(shafts && withShaft > 0, std::to_string(withShaft) + " entrance shafts lead up to the surface");
    Check(sealed, "water sits in sealed basins");
    Check(deep, "rooms are at least 8 blocks below the ground");
    Check(pitsOk && coveredPits > 0 && openPits > 0,
          std::to_string(coveredPits) + " covered and " + std::to_string(openPits) + " open corridor pits, with spikes along the bottom");
    Check(vaultsOk && vaults > 0, std::to_string(vaults) + " vault pedestals ringed by crumbling floor over spikes");
    Check(vaultChestsOk, "each with a chest on it, full of loot");
    {
        // Archives keep a chest by the reading table, with a few useful things in it.
        int archiveChests = 0;
        bool archiveLoot = true;
        for (const auto& [region, layout] : dungeons)
            for (const auto& r : layout.rooms)
                if (r.type == Dungeons::RoomType::Archive) {
                    const glm::ivec3 c = (r.inner.min + r.inner.max) / 2;
                    const glm::ivec3 at(c.x - 1, r.inner.min.y, c.z);
                    if (world.Get(at).type != BlockType::CHEST) continue;
                    archiveChests++;
                    archiveLoot &= !Dungeons::LootFor(world.terrain, at).empty();
                }
        Check(archiveChests > 0 && archiveLoot, std::to_string(archiveChests) + " archive chests, each with loot");
        Check(Dungeons::LootFor(world.terrain, { 5, 80, 5 }).empty(), "no loot for a chest that isn't in a dungeon (someone's own)");
        const auto& layout = dungeons[0].second;
        const glm::ivec3 any = layout.rooms[0].inner.min;
        Check(Dungeons::LootFor(world.terrain, any) == Dungeons::LootFor(world.terrain, any), "the same chest always rolls the same loot");
    }
    Check(floodsOk && floods > 0, std::to_string(floods) + " flood chambers with a plate, sluice gates and a full reservoir");
    Check(floodWired, "each flood chamber's plate sets a latch gate in the ceiling, which its 9 sluice gates listen to, and a lever resets it");
    Check(floodLevers * 2 >= floods, std::to_string(floodLevers) + " of them have the lever in the wall (a doorway can take its place)");
    Check(leverVaults > 0 && leverVaultsBuilt, std::to_string(leverVaults) + " lever vaults have four levers, a floor gate, and a chest of rich loot under it");
    Check(leverVaultsWired, "each one's gate opens for its lever pattern and no other (of the 16)");
    Check(leverVaultsHinted, "and has a lamp over each lever that should be on, and none over the others");
    Check(gardens, "gardens are planted all over");
    Check(archives, "archive shelves are exactly where the layout puts them (all along the walls, gaps and doorways aside)");
    Check(pumps && pumpRooms > 0, std::to_string(pumpRooms) + " pump rooms have a pump under water with a pipe beside it");
    std::string mix;
    for (const auto& [name, count] : typeCounts) mix += (mix.empty() ? "" : ", ") + name + " " + std::to_string(count);
    std::cout << "        rooms: " << mix << std::endl;

    std::cout << "Determinism and cost" << std::endl;
    {
        const auto& layout = dungeons[0].second;
        const glm::ivec3 c = Coords::ChunkOf(Dungeons::Box(layout.rooms[0].inner).min);
        TerrainGenerator::ColumnHeights heights;
        world.terrain.ComputeColumnHeights(c.x, c.z, heights);
        TerrainGenerator::Blocks a, b;
        world.terrain.GenerateChunk(c, heights, a);
        world.terrain.GenerateChunk(c, heights, b);
        Check(a == b, "a chunk with a dungeon in it generates the same twice");
        std::cout << "        " << world.generated << " chunks generated, " << world.generateSeconds * 1000.0 / world.generated << " ms each on average" << std::endl;
    }

    std::cout << (failures ? "DUNGEON TEST FAILED (" : "ALL DUNGEON CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
