// Dwarf camps and mines (see Dwarves.h).
#include "world/Dwarves.h"

#include <algorithm>
#include <climits>
#include <cmath>

#include "world/Noise.h"
#include "world/TerrainGenerator.h"

namespace Dwarves {
namespace {

constexpr uint32_t kSeed = 0xD3A2Fu;
constexpr int S = Chunk::CHUNK_SIZE;

int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Dry, above the sea, out of lakes and rivers, and level within `spread` blocks
// across the samples round `c` out to `radius`.
bool GoodGround(const TerrainGenerator& t, const glm::ivec2& c, int radius, int spread, int& floor) {
    int lo = INT_MAX, hi = INT_MIN;
    for (int dx = -radius; dx <= radius; dx += radius)
        for (int dz = -radius; dz <= radius; dz += radius) {
            const int x = c.x + dx, z = c.y + dz;
            if (t.LakeLevelAt(x, z) != INT_MIN || t.InRiver(x, z, 4.0f)) return false;
            const int row = t.ColumnAt(x, z).FirstOpenRow();
            if (row <= TerrainGenerator::kSeaLevel + 2 || row >= TerrainGenerator::kRockLine - 4) return false; // (dry, below the bare peaks)
            lo = std::min(lo, row), hi = std::max(hi, row);
        }
    floor = t.ColumnAt(c.x, c.y).FirstOpenRow();
    return hi - lo <= spread;
}

RegionSite StrongholdIn(const TerrainGenerator& t, int rx, int rz, RegionSite out) {
    static const glm::ivec3 kDirs[4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    const int margin = 40;
    const glm::ivec2 c(rx * kRegionSize + margin + static_cast<int>(out.seed % (kRegionSize - 2 * margin)),
                       rz * kRegionSize + margin + static_cast<int>((out.seed >> 10) % (kRegionSize - 2 * margin)));
    int ground = 0;
    if (!GoodGround(t, c, kHallHalf, 14, ground)) return out;
    out.gate = kDirs[(out.seed >> 20) % 4];
    const glm::ivec3 hall(c.x, ground - kHallDepth, c.y);
    // The stairway climbs a block a step from the hall's wall until it's in the open,
    // which mustn't be water.
    for (int k = 1; k < 60; k++) {
        const glm::ivec3 p = hall + out.gate * (kHallHalf + 1 + k);
        if (hall.y + k >= t.ColumnAt(p.x, p.z).FirstOpenRow()) {
            if (t.LakeLevelAt(p.x, p.z) != INT_MIN || t.InRiver(p.x, p.z, 3.0f) || hall.y + k <= TerrainGenerator::kSeaLevel + 1) return out;
            out.stairs = k;
            break;
        }
    }
    if (out.stairs == 0) return out;
    out.exists = true;
    Site& s = out.site;
    s.kind = SiteKind::Stronghold;
    s.centre = hall;
    const glm::ivec3 side(out.gate.z, 0, -out.gate.x);
    s.spawns.push_back(hall - out.gate * 7); // (the king's: before his throne)
    for (int a : { -4, 0, 4 })
        for (int b : { -3, 3 }) s.spawns.push_back(hall + out.gate * a + side * b);
    for (int a : { -2, 2, 6 }) s.spawns.push_back(hall + out.gate * a);
    // (In the vault, behind the throne.)
    s.chests.push_back(hall - out.gate * (kHallHalf + 3) + side * 2 + glm::ivec3(0, 1, 0));
    s.chests.push_back(hall - out.gate * (kHallHalf + 3) + side * 4 + glm::ivec3(0, 1, 0));
    s.kings = 1, s.warriors = 6, s.crossbows = 3;
    return out;
}

} // namespace

RegionSite SiteIn(const TerrainGenerator& terrain, int rx, int rz) {
    RegionSite out;
    if (terrain.GetType() != TerrainGenerator::Type::Normal) return out;
    out.seed = Noise::Hash(rx, 11, rz, kSeed);
    const float roll = Noise::Random01(rx, 12, rz, kSeed);
    const bool camp = roll < 0.32f, mine = !camp && roll < 0.58f, stronghold = !camp && !mine && roll < 0.62f;
    if (stronghold) return StrongholdIn(terrain, rx, rz, out);
    if (!camp && !mine) return out;
    const int margin = 28;
    const glm::ivec2 c(rx * kRegionSize + margin + static_cast<int>(out.seed % (kRegionSize - 2 * margin)),
                       rz * kRegionSize + margin + static_cast<int>((out.seed >> 10) % (kRegionSize - 2 * margin)));
    int floor = 0;
    if (!GoodGround(terrain, c, camp ? kCampRadius : 3, camp ? 3 : 2, floor)) return out;
    out.exists = true;
    Site& s = out.site;
    static const glm::ivec3 kDirs[4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    if (camp) {
        s.kind = SiteKind::Camp;
        s.centre = glm::ivec3(c.x, floor, c.y);
        out.gate = kDirs[(out.seed >> 20) % 4];
        for (int k = 0; k < 6; k++) {
            const float a = k * 1.0472f + 0.5f;
            s.spawns.push_back(s.centre + glm::ivec3(static_cast<int>(std::lround(std::cos(a) * 3.0f)), 0, static_cast<int>(std::lround(std::sin(a) * 3.0f))));
        }
        const glm::ivec3 side(out.gate.z, 0, -out.gate.x);
        // One camp in four is a market, of friendly dwarves (and no loot).
        if ((out.seed >> 24) % 4 == 0) {
            s.kind = SiteKind::Market;
            s.traders = 1, s.guards = 3;
        } else {
            s.chests.push_back(s.centre - out.gate * 2 + side * 2);
            s.warriors = 4, s.crossbows = 2, s.chiefs = 1;
        }
    }
    else {
        s.kind = SiteKind::Mine;
        s.centre = glm::ivec3(c.x, floor, c.y);
        out.bottom = floor - kMineDepth;
        const int count = 2 + static_cast<int>((out.seed >> 12) % 3);
        for (int k = 0; k < count; k++) {
            const glm::ivec3 d = kDirs[(k + (out.seed >> 16)) % 4];
            const int length = 14 + static_cast<int>(Noise::Random01(rx, 20 + k, rz, kSeed) * 14.0f);
            out.tunnels.emplace_back(d, length);
            for (int i = 4; i < length; i += 6) s.spawns.push_back(glm::ivec3(c.x, out.bottom, c.y) + d * (2 + i));
            s.chests.push_back(glm::ivec3(c.x, out.bottom, c.y) + d * (2 + length - 1));
        }
        s.warriors = 4, s.crossbows = 2, s.chiefs = 0;
    }
    return out;
}

std::vector<Site> SitesNear(const TerrainGenerator& terrain, const glm::vec3& pos, float range) {
    std::vector<Site> out;
    const int r0x = FloorDiv(static_cast<int>(std::floor(pos.x - range)), kRegionSize), r1x = FloorDiv(static_cast<int>(std::floor(pos.x + range)), kRegionSize);
    const int r0z = FloorDiv(static_cast<int>(std::floor(pos.z - range)), kRegionSize), r1z = FloorDiv(static_cast<int>(std::floor(pos.z + range)), kRegionSize);
    for (int rx = r0x; rx <= r1x; rx++)
        for (int rz = r0z; rz <= r1z; rz++) {
            RegionSite r = SiteIn(terrain, rx, rz);
            if (r.exists && glm::distance(glm::vec2(pos.x, pos.z), glm::vec2(r.site.centre.x, r.site.centre.z)) <= range) out.push_back(r.site);
        }
    return out;
}

namespace {

// A stronghold: its hall, pillars, throne, vault and stairway (see Dwarves.h).
template <typename Set, typename In, typename At>
void StampStronghold(const TerrainGenerator& terrain, const RegionSite& r, const glm::ivec3& lo, const glm::ivec3& hi, Set set, In in, At at) {
    const glm::ivec3 c = r.site.centre, g = r.gate, side(g.z, 0, -g.x);
    // Everything's within the hall's reach or along the stairway: skip a chunk outside both.
    const glm::ivec3 far = c + g * (kHallHalf + 2 + r.stairs) + glm::ivec3(0, r.stairs, 0);
    const glm::ivec3 bmin = glm::min(c - glm::ivec3(16, 4, 16), far - glm::ivec3(4, 0, 4));
    const glm::ivec3 bmax = glm::max(c + glm::ivec3(16, 12, 16), far + glm::ivec3(4, 12, 4));
    if (glm::any(glm::lessThan(hi, bmin)) || glm::any(glm::greaterThan(lo, bmax))) return;
    const Block kAir{}, kBrick(BlockType::STONE_BRICK), kMossy(BlockType::MOSSY_BRICK), kLamp(BlockType::LAMP), kPlanks(BlockType::PLANKS),
        kLog(BlockType::LOG), kChest(BlockType::CHEST), kIron(BlockType::IRON_ORE), kCoal(BlockType::COAL_ORE);
    auto brick = [&](const glm::ivec3& p) { return Noise::Random01(p.x, p.y, p.z, r.seed) < 0.18f ? kMossy : kBrick; };
    // (along: towards the stairway; across: to the side.)
    auto cell = [&](int along, int y, int across) { return c + g * along + side * across + glm::ivec3(0, y, 0); };
    const int H = kHallHalf + 1; // the wall
    // Rock round it rich in ore, then the hall's shell, and the hall itself.
    for (int a = -H - 3; a <= H + 3; a++)
        for (int b = -H - 3; b <= H + 3; b++)
            for (int y = -3; y <= 10; y++) {
                const glm::ivec3 p = cell(a, y, b);
                if (!in(p)) continue;
                const bool inside = std::abs(a) < H && std::abs(b) < H && y >= 0 && y <= 7;
                const bool shell = !inside && std::abs(a) <= H && std::abs(b) <= H && y >= -1 && y <= 8;
                if (inside) set(p, kAir);
                else if (shell) set(p, brick(p));
                else if (at(p).type == BlockType::STONE) {
                    const float n = Noise::Random01(p.x * 3 + 1, p.y, p.z * 3 + 2, r.seed);
                    if (n < 0.14f) set(p, kIron);
                    else if (n < 0.22f) set(p, kCoal);
                }
            }
    // Pillars (two by two), each with a lamp on it.
    for (int pa : { -6, 5 })
        for (int pb : { -6, 5 })
            for (int da = 0; da < 2; da++)
                for (int db = 0; db < 2; db++)
                    for (int y = 0; y <= 7; y++) set(cell(pa + da, y, pb + db), (y == 4 && da == 0 && db == 0) ? kLamp : brick(cell(pa + da, y, pb + db)));
    // A plank walk from the stairway to the throne.
    for (int a = -H + 3; a < H; a++)
        for (int b = -1; b <= 1; b++) set(cell(a, -1, b), kPlanks);
    // The dais and throne at the far end, lamps either side.
    for (int a = -H + 1; a <= -H + 3; a++)
        for (int b = -3; b <= 3; b++) set(cell(a, 0, b), kBrick);
    set(cell(-H + 2, 1, 0), kPlanks);
    for (int y = 1; y <= 3; y++) set(cell(-H + 1, y, 0), kLog);
    for (int b : { -2, 2 }) set(cell(-H + 1, 1, b), kLamp);
    // The vault behind the wall, off to one side, its doorway from the dais.
    for (int a = -H - 3; a <= -H + 1; a++)
        for (int b = 1; b <= 5; b++)
            for (int y = 0; y <= 4; y++) {
                const bool room = a >= -H - 2 && a <= -H - 1 && b >= 2 && b <= 4 && y >= 1 && y <= 3;
                if (a > -H) continue;
                set(cell(a, y, b), room ? kAir : brick(cell(a, y, b)));
            }
    set(cell(-H, 1, 3), kAir);
    set(cell(-H, 2, 3), kAir);
    set(cell(-H - 2, 3, 3), kLamp);
    for (const auto& chest : r.site.chests) set(chest, kChest);
    // The stairway: three wide and four high, a step up each block, walled and roofed
    // in brick while it's underground; then a gatehouse in the open.
    for (int k = 0; k <= r.stairs; k++) {
        const int floor = c.y + k;
        for (int b = -2; b <= 2; b++)
            for (int y = -1; y <= 4; y++) {
                const glm::ivec3 p = cell(H + k, floor - c.y + y, b);
                if (!in(p)) continue;
                const bool open = std::abs(b) <= 1 && y >= 0 && y <= 3;
                const int ground = terrain.ColumnAt(p.x, p.z).FirstOpenRow();
                if (open) set(p, kAir);
                else if (p.y < ground + 1 || y == -1) set(p, brick(p));
            }
    }
    const int top = c.y + r.stairs;
    for (int b : { -2, 2 }) {
        for (int y = 0; y <= 4; y++) set(cell(H + r.stairs, top - c.y + y, b), kBrick);
        set(cell(H + r.stairs, top - c.y + 5, b), kLamp);
    }
    for (int b = -1; b <= 1; b++) set(cell(H + r.stairs, top - c.y + 4, b), kBrick);
}

} // namespace

void Stamp(const TerrainGenerator& terrain, const glm::ivec3& coords, Blocks& out) {
    const glm::ivec3 lo = coords * S, hi = lo + S - 1;
    auto in = [&](const glm::ivec3& p) { return glm::all(glm::greaterThanEqual(p, lo)) && glm::all(glm::lessThanEqual(p, hi)); };
    auto at = [&](const glm::ivec3& p) -> Block& {
        const glm::ivec3 l = p - lo;
        return out[l.x + S * (l.y + S * l.z)];
    };
    auto set = [&](const glm::ivec3& p, const Block& b) {
        if (in(p)) at(p) = b;
    };
    const Block kAir{}, kLog(BlockType::LOG), kPlanks(BlockType::PLANKS), kDirt(BlockType::DIRT), kLamp(BlockType::LAMP), kChest(BlockType::CHEST);
    EdgeData slab;
    for (int e = 0; e < 4; e++) slab.SetTopY(e, 4);
    const int r0x = FloorDiv(lo.x - 80, kRegionSize), r1x = FloorDiv(hi.x + 80, kRegionSize);
    const int r0z = FloorDiv(lo.z - 80, kRegionSize), r1z = FloorDiv(hi.z + 80, kRegionSize);
    for (int rx = r0x; rx <= r1x; rx++)
        for (int rz = r0z; rz <= r1z; rz++) {
            const RegionSite r = SiteIn(terrain, rx, rz);
            if (!r.exists) continue;
            const Site& s = r.site;
            const glm::ivec3 c = s.centre;
            if (s.kind == SiteKind::Stronghold) {
                StampStronghold(terrain, r, lo, hi, set, in, at);
                continue;
            }
            if (s.kind != SiteKind::Mine) {
                if (c.x + 9 < lo.x || c.x - 9 > hi.x || c.z + 9 < lo.z || c.z - 9 > hi.z || c.y + 6 < lo.y || c.y - 4 > hi.y) continue;
                const glm::vec2 gate(r.gate.x, r.gate.z);
                for (int dx = -8; dx <= 8; dx++)
                    for (int dz = -8; dz <= 8; dz++) {
                        const float d = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                        if (d > 8.0f) continue;
                        const glm::ivec3 col = c + glm::ivec3(dx, 0, dz);
                        // Cleared and levelled: trodden earth inside, the ground made up.
                        if (d <= 7.5f) {
                            for (int y = 0; y <= 6; y++) set(col + glm::ivec3(0, y, 0), kAir);
                            for (int y = -3; y <= -1; y++) {
                                const glm::ivec3 p = col + glm::ivec3(0, y, 0);
                                if (in(p) && (y == -1 || !at(p).IsFullBlock())) set(p, kDirt);
                            }
                        }
                        // The palisade, with a gap for the gate.
                        if (d >= 6.5f && d < 7.5f && glm::dot(glm::normalize(glm::vec2(dx, dz)), gate) < 0.92f) {
                            const int tall = 2 + ((dx * 7 + dz * 13) & 1);
                            for (int y = 0; y < tall; y++) set(col + glm::ivec3(0, y, 0), kLog);
                        }
                    }
                // Huts round the fire (not on the gate side), doors facing it.
                for (int k = 0; k < 3; k++) {
                    const float a = std::atan2(static_cast<float>(-r.gate.z), static_cast<float>(-r.gate.x)) + (k - 1) * 1.6f;
                    const glm::ivec3 h = c + glm::ivec3(static_cast<int>(std::lround(std::cos(a) * 4.0f)), 0, static_cast<int>(std::lround(std::sin(a) * 4.0f)));
                    const glm::vec2 toFire = glm::normalize(glm::vec2(c.x - h.x, c.z - h.z) + 1e-4f);
                    for (int dx = -1; dx <= 1; dx++)
                        for (int dz = -1; dz <= 1; dz++) {
                            const glm::ivec3 p = h + glm::ivec3(dx, 0, dz);
                            const bool door = (dx != 0 || dz != 0) && glm::dot(glm::normalize(glm::vec2(dx, dz)), toFire) > 0.9f;
                            if ((dx != 0 || dz != 0) && !door) set(p, (std::abs(dx) == 1 && std::abs(dz) == 1) ? kLog : kPlanks);
                            set(p + glm::ivec3(0, 1, 0), Block(BlockType::PLANKS, slab));
                        }
                }
                set(c, Block::Plant(BlockType::CAMPFIRE, 0));
                for (const auto& chest : s.chests) set(chest, kChest);
                if (s.kind == SiteKind::Market) {
                    // Lamps on posts either side of the gate.
                    const glm::ivec3 side(r.gate.z, 0, -r.gate.x);
                    for (int k : { -2, 2 }) {
                        const glm::ivec3 post = c + r.gate * 6 + side * k;
                        for (int y = 0; y < 3; y++) set(post + glm::ivec3(0, y, 0), kLog);
                        set(post + glm::ivec3(0, 3, 0), kLamp);
                    }
                }
            }
            else {
                const int bottom = r.bottom;
                if (c.y + 5 < lo.y || bottom - 2 > hi.y) continue;
                // The shaft, its ladder, and the headframe over it.
                if (c.x + 3 >= lo.x && c.x - 3 <= hi.x && c.z + 3 >= lo.z && c.z - 3 <= hi.z) {
                    for (int y = bottom; y <= c.y + 3; y++)
                        for (int dx = -1; dx <= 1; dx++)
                            for (int dz = -1; dz <= 1; dz++) set(glm::ivec3(c.x + dx, y, c.z + dz), kAir);
                    for (int y = bottom; y < c.y + 1; y++) set(glm::ivec3(c.x - 1, y, c.z), Block(BlockType::LADDER));
                    for (int y = bottom - 1; y >= bottom - 1; y--)
                        for (int dx = -1; dx <= 1; dx++)
                            for (int dz = -1; dz <= 1; dz++) {
                                const glm::ivec3 p(c.x + dx, y, c.z + dz);
                                if (in(p) && !at(p).IsFullBlock()) set(p, kPlanks);
                            }
                    for (int dx : { -2, 2 })
                        for (int dz : { -2, 2 })
                            for (int y = c.y - 1; y <= c.y + 2; y++) set(glm::ivec3(c.x + dx, y, c.z + dz), kLog);
                    for (int dx = -2; dx <= 2; dx++)
                        for (int dz = -2; dz <= 2; dz++)
                            if (std::max(std::abs(dx), std::abs(dz)) == 2) set(glm::ivec3(c.x + dx, c.y + 3, c.z + dz), kPlanks);
                }
                // The tunnels: carved, framed, lit, ore in the walls, a chest at the end.
                for (size_t k = 0; k < r.tunnels.size(); k++) {
                    const glm::ivec3 d = r.tunnels[k].first;
                    const int length = r.tunnels[k].second;
                    const glm::ivec3 side(d.z, 0, -d.x);
                    for (int i = 0; i < length; i++) {
                        const glm::ivec3 base = glm::ivec3(c.x, bottom, c.z) + d * (2 + i);
                        if (base.x + 3 < lo.x || base.x - 3 > hi.x || base.z + 3 < lo.z || base.z - 3 > hi.z) continue;
                        for (int w = -1; w <= 1; w++) {
                            for (int y = 0; y <= 2; y++) set(base + side * w + glm::ivec3(0, y, 0), kAir);
                            const glm::ivec3 floorCell = base + side * w - glm::ivec3(0, 1, 0);
                            if (in(floorCell) && !at(floorCell).IsFullBlock()) set(floorCell, kPlanks); // (across a cave)
                        }
                        const bool frame = i % 5 == 2;
                        for (int w : { -2, 2 })
                            for (int y = 0; y <= 2; y++) {
                                const glm::ivec3 wall = base + side * w + glm::ivec3(0, y, 0);
                                if (frame) set(wall, kLog);
                                else if (in(wall) && at(wall).type == BlockType::STONE) {
                                    const float ore = Noise::Random01(wall.x, wall.y, wall.z, kSeed ^ 0x0AEu);
                                    if (ore < 0.08f) set(wall, Block(ore < 0.03f ? BlockType::IRON_ORE : BlockType::COAL_ORE));
                                }
                            }
                        if (frame)
                            for (int w = -2; w <= 2; w++) set(base + side * w + glm::ivec3(0, 3, 0), (w == 0 && i % 10 == 2) ? kLamp : kPlanks);
                    }
                }
                for (const auto& chest : s.chests) set(chest, kChest);
            }
        }
}

std::vector<std::pair<BlockType, uint32_t>> LootFor(const TerrainGenerator& terrain, const glm::ivec3& pos) {
    std::vector<std::pair<BlockType, uint32_t>> loot;
    const RegionSite r = SiteIn(terrain, FloorDiv(pos.x, kRegionSize), FloorDiv(pos.z, kRegionSize));
    if (!r.exists || std::find(r.site.chests.begin(), r.site.chests.end(), pos) == r.site.chests.end()) return loot;
    constexpr uint32_t kBlock = Block::kUnitsPerBlock;
    auto roll = [&](int salt, int least, int most) {
        return static_cast<uint32_t>(least + static_cast<int>(Noise::Random01(pos.x, pos.y + salt, pos.z, r.seed) * (most - least + 1))) * kBlock;
    };
    if (r.site.kind == SiteKind::Stronghold) {
        loot = { { BlockType::STEEL_INGOT, roll(1, 3, 6) }, { BlockType::IRON_INGOT, roll(2, 6, 12) }, { BlockType::TNT, roll(3, 2, 4) },
                 { BlockType::ARROW, roll(4, 8, 16) }, { BlockType::BREAD, roll(5, 3, 6) } };
    }
    else if (r.site.kind == SiteKind::Camp) {
        loot = { { BlockType::BREAD, roll(1, 2, 5) }, { BlockType::COOKED_VENISON, roll(2, 1, 3) }, { BlockType::ARROW, roll(3, 4, 12) },
                 { BlockType::IRON_INGOT, roll(4, 1, 3) }, { BlockType::HIDE, roll(5, 1, 3) } };
        if (Noise::Random01(pos.x, pos.y + 6, pos.z, r.seed) < 0.4f) loot.push_back({ BlockType::STEEL_INGOT, kBlock });
    }
    else {
        loot = { { BlockType::COAL, roll(1, 4, 10) }, { BlockType::IRON_ORE, roll(2, 2, 6) }, { BlockType::IRON_INGOT, roll(3, 1, 3) },
                 { BlockType::TNT, roll(4, 1, 2) }, { BlockType::BREAD, roll(5, 1, 3) } };
        if (Noise::Random01(pos.x, pos.y + 6, pos.z, r.seed) < 0.5f) loot.push_back({ BlockType::STEEL_INGOT, roll(7, 1, 2) });
    }
    return loot;
}

} // namespace Dwarves
