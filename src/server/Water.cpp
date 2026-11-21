#include "server/Water.h"

#include <algorithm>
#include <cassert>
#include <chrono>

#include "server/ServerWorld.h"

namespace {

const glm::ivec3 kHorizontal[4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
const glm::ivec3 kUp(0, 1, 0), kDown(0, -1, 0);
// Water shallower than this (1/8 of a block) only flows onto dry ground when it stands
// at least this high above that ground, or deeper water right behind it keeps it fed.
// So spilled water ends as a shallow puddle instead of an endless film, while a lake,
// or a shallow flat at lake level, still drains into a channel dug beside it. Water
// levels with other water at any depth.
constexpr int kMinSpreadVolume = Block::kMaxWaterVolume / 8;
// How far along connected water a cell levels out.
constexpr int kReach = 6;

// Water crossing between cells goes through the open part of a partial block: its four
// quarters (a stepped block's quarters, a slope's corners), each open where it's lower
// than the water. Full water cells and air are open everywhere.
struct Quarters {
    bool open[2][2]; // [x][z]
};
Quarters OpenQuarters(const Block& b, int level) {
    constexpr int kEdgeAt[2][2] = { { 2, 0 }, { 1, 3 } }; // EdgeData index of quarter [x][z]
    Quarters q;
    for (int x = 0; x < 2; x++)
        for (int z = 0; z < 2; z++)
            q.open[x][z] = !b.IsPartial() || b.edgeData.GetTopY(kEdgeAt[x][z]) * (Block::kMaxWaterVolume / 8) < level;
    return q;
}
// The quarters on the face toward horizontal direction d.
bool OnFace(int x, int z, const glm::ivec3& d) {
    return (d.x > 0 && x == 1) || (d.x < 0 && x == 0) || (d.z > 0 && z == 1) || (d.z < 0 && z == 0);
}
// Whether water at `level` can get through the cell's face toward d (in or out).
bool OpenFace(const Block& b, const glm::ivec3& d, int level) {
    const Quarters q = OpenQuarters(b, level);
    for (int x = 0; x < 2; x++)
        for (int z = 0; z < 2; z++)
            if (q.open[x][z] && OnFace(x, z, d)) return true;
    return false;
}
// Whether water at `level` can go right through the cell, in by the face toward
// `from` and out by the face toward `to`: open quarters joined side by side.
bool PassesThrough(const Block& b, const glm::ivec3& from, const glm::ivec3& to, int level) {
    if (!b.IsPartial()) return true;
    const Quarters q = OpenQuarters(b, level);
    bool reached[2][2] = {};
    for (int x = 0; x < 2; x++)
        for (int z = 0; z < 2; z++) reached[x][z] = q.open[x][z] && OnFace(x, z, from);
    for (int pass = 0; pass < 3; pass++) // spread to open quarters beside reached ones
        for (int x = 0; x < 2; x++)
            for (int z = 0; z < 2; z++)
                if (q.open[x][z] && !reached[x][z] && (reached[1 - x][z] || reached[x][1 - z])) reached[x][z] = true;
    for (int x = 0; x < 2; x++)
        for (int z = 0; z < 2; z++)
            if (reached[x][z] && OnFace(x, z, to)) return true;
    return false;
}

// Sets the water in a cell, keeping a partial block (waterlogged) if it is one.
void SetWater(ServerWorld& world, const glm::ivec3& p, const Block& cell, int volume) {
    // Every caller conserves volume; this catches a bug before it can wrap the byte
    // (which would silently create water).
    assert(volume >= 0 && volume <= cell.WaterCapacity());
    world.SetBlock(p.x, p.y, p.z, cell.WithWater(static_cast<uint8_t>(std::clamp(volume, 0, static_cast<int>(cell.WaterCapacity())))));
}

// A cell with nothing left to move is at rest: draw its true volume (see Block::shown).
void SnapShown(ServerWorld& world, const glm::ivec3& p, const Block& cell) {
    if (cell.shown == cell.water) return;
    Block b = cell;
    b.shown = b.water;
    world.SetBlock(p.x, p.y, p.z, b);
}

} // namespace

void WaterSimulation::Activate(const glm::ivec3& pos) {
    // Everything whose update reads this cell: the cells above and below, and cells
    // up to kReach away horizontally (levelling looks that far along water).
    active.insert(pos);
    active.insert(pos + kUp);
    active.insert(pos + kDown);
    for (const auto& d : kHorizontal)
        for (int step = 1; step <= kReach; step++) active.insert(pos + d * step);
    activeCount.store(active.size(), std::memory_order_relaxed);
    Wake();
}

void WaterSimulation::AddPump(const glm::ivec3& pos) {
    pumps.insert(pos);
    pumpCount.store(pumps.size(), std::memory_order_relaxed);
    Wake();
}

void WaterSimulation::RemovePump(const glm::ivec3& pos) {
    pumps.erase(pos);
    pumpBudget.erase(pos);
    pumpUse.erase(pos);
    pumpCount.store(pumps.size(), std::memory_order_relaxed);
}

void WaterSimulation::RunPumps(ServerWorld& world) {
    std::vector<glm::ivec3> gone;
    for (const auto& pos : pumps) {
        Block block;
        // Unloaded (a saved pump is added again when its chunk loads) or replaced.
        if (!world.GetBlock(pos.x, pos.y, pos.z, block)) {
            gone.push_back(pos);
            continue;
        }
        if (block.type != BlockType::PUMP) {
            gone.push_back(pos);
            continue;
        }
        if (limited && !NearCenter(pos, distance)) continue;
        RunPump(world, pos);
    }
    for (const auto& pos : gone) {
        pumps.erase(pos);
        pumpBudget.erase(pos);
    }
    pumpCount.store(pumps.size(), std::memory_order_relaxed);
}

void WaterSimulation::RunPump(ServerWorld& world, const glm::ivec3& pump) {
    const glm::ivec3 faces[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    auto typeAt = [&](const glm::ivec3& p, Block& b) { return world.GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT; };

    // Intake: water in the cells around the pump.
    struct Cell {
        glm::ivec3 pos;
        Block block;
    };
    std::vector<Cell> intake;
    std::vector<glm::ivec3> frontier;
    for (const auto& d : faces) {
        Block b;
        const BlockType t = typeAt(pump + d, b);
        if (t == BlockType::PIPE) frontier.push_back(pump + d);
        else if (t != BlockType::COUNT && b.HasWater()) intake.push_back({ pump + d, b });
    }
    if (intake.empty() || frontier.empty()) return;

    // The pipe network, and its ends: pipes joining only one other pipe or pump.
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> seen(frontier.begin(), frontier.end());
    std::vector<Cell> outlets;
    auto isIntake = [&](const glm::ivec3& p) {
        for (const auto& c : intake)
            if (c.pos == p) return true;
        return false;
    };
    for (size_t i = 0; i < frontier.size() && seen.size() <= kMaxPipes; i++) {
        const glm::ivec3 pipe = frontier[i];
        int joins = 0;
        std::vector<Cell> open;
        for (const auto& d : faces) {
            const glm::ivec3 n = pipe + d;
            Block b;
            const BlockType t = typeAt(n, b);
            if (t == BlockType::PIPE || t == BlockType::PUMP) {
                joins++;
                if (t == BlockType::PIPE && seen.insert(n).second) frontier.push_back(n);
            }
            else if (t != BlockType::COUNT && b.CanHoldWater() && b.WaterVolume() < b.WaterCapacity() && !isIntake(n)) {
                open.push_back({ n, b });
            }
        }
        if (joins <= 1) outlets.insert(outlets.end(), open.begin(), open.end());
    }
    if (outlets.empty()) return;
    // A cell beside two pipe ends counts once.
    std::sort(outlets.begin(), outlets.end(), [](const Cell& a, const Cell& b) {
        return a.pos.x != b.pos.x ? a.pos.x < b.pos.x : a.pos.y != b.pos.y ? a.pos.y < b.pos.y : a.pos.z < b.pos.z;
    });
    outlets.erase(std::unique(outlets.begin(), outlets.end(), [](const Cell& a, const Cell& b) { return a.pos == b.pos; }), outlets.end());

    // How much can move: the pump's rate, the water around it, the room at the ends.
    int available = 0, room = 0;
    for (const auto& c : intake) available += c.block.WaterVolume();
    for (const auto& c : outlets) room += c.block.WaterCapacity() - c.block.WaterVolume();
    int amount = std::min({ kPumpRate, available, room });
    if (amount <= 0) return;
    // As much as its air allows: each unit costs its lift.
    int top = outlets.front().pos.y;
    for (const auto& c : outlets) top = std::max(top, c.pos.y);
    const int lift = std::max(1, top - pump.y);
    PumpUse& use = pumpUse[pump];
    use.wanted = true;
    float& budget = pumpBudget[pump];
    amount = std::min(amount, static_cast<int>(budget / static_cast<float>(lift)));
    if (amount <= 0) return;
    budget -= static_cast<float>(amount * lift);
    use.lifted += static_cast<float>(amount * lift);

    // Take it from the highest water first (drawing down the surface).
    std::sort(intake.begin(), intake.end(), [](const Cell& a, const Cell& b) { return a.block.WaterLevel() > b.block.WaterLevel(); });
    int toTake = amount;
    for (const auto& c : intake) {
        if (toTake == 0) break;
        const int take = std::min(toTake, static_cast<int>(c.block.WaterVolume()));
        SetWater(world, c.pos, c.block, c.block.WaterVolume() - take);
        toTake -= take;
    }

    // Equal shares to the ends, each capped by its room; what doesn't fit goes round
    // to the others.
    std::vector<int> given(outlets.size(), 0);
    while (amount > 0) {
        int open = 0;
        for (size_t i = 0; i < outlets.size(); i++)
            if (given[i] < outlets[i].block.WaterCapacity() - outlets[i].block.WaterVolume()) open++;
        const int share = std::max(1, amount / open);
        for (size_t i = 0; i < outlets.size() && amount > 0; i++) {
            const int roomLeft = outlets[i].block.WaterCapacity() - outlets[i].block.WaterVolume() - given[i];
            const int g = std::min({ share, roomLeft, amount });
            given[i] += g;
            amount -= g;
        }
    }
    for (size_t i = 0; i < outlets.size(); i++)
        if (given[i] > 0) SetWater(world, outlets[i].pos, outlets[i].block, outlets[i].block.WaterVolume() + given[i]);
}

bool WaterSimulation::IsPuddle(const ServerWorld& world, const glm::ivec3& pos, const Block& block) {
    if (!block.HasWater() || block.WaterVolume() > kPuddleVolume || block.WaterVolume() >= block.WaterCapacity()) return false;
    // Not under water, and not the thin top layer of a lake or pool (on more water).
    Block above, below;
    if (world.GetBlock(pos.x, pos.y + 1, pos.z, above) && above.HasWater()) return false;
    return !world.GetBlock(pos.x, pos.y - 1, pos.z, below) || !below.HasWater();
}

void WaterSimulation::RandomTicks(ServerWorld& world) {
    // The chunks in the simulation area (once each where players' areas overlap).
    // Read under the edit mutex since SetSimulationArea changes them.
    randomChunks.clear();
    {
        std::lock_guard<std::recursive_mutex> lock(world.GetEditMutex());
        if (!limited) return; // no players to tick around
        std::unordered_set<glm::ivec3, Coords::IVec3Hash> seen;
        for (const auto& center : centers)
            for (int x = -distance; x <= distance; x++)
                for (int y = -distance; y <= distance; y++)
                    for (int z = -distance; z <= distance; z++)
                        if (seen.insert(center + glm::ivec3(x, y, z)).second) randomChunks.push_back(center + glm::ivec3(x, y, z));
    }

    // Look at random cells without the edit mutex (block reads don't need it)...
    constexpr int S = Chunk::CHUNK_SIZE;
    std::uniform_int_distribution<int> local(0, S - 1);
    const bool rain = IsRaining();
    puddles.clear();
    wet.clear();
    for (const auto& chunk : randomChunks)
        for (int i = 0; i < kRandomTicksPerChunk; i++) {
            const glm::ivec3 pos = chunk * S + glm::ivec3(local(random), local(random), local(random));
            Block block;
            if (!world.GetBlock(pos.x, pos.y, pos.z, block) || !block.HasWater()) continue;
            if (rain) wet.push_back(pos);                                // rain lands in it
            else if (block.WaterVolume() <= kPuddleVolume) puddles.push_back(pos); // may dry up
        }
    if (puddles.empty() && wet.empty()) return;

    // ...then check the few candidates properly and dry them up under it.
    std::lock_guard<std::recursive_mutex> lock(world.GetEditMutex());
    for (const auto& pos : wet) RainOn(world, pos);
    for (const auto& pos : puddles) {
        Block block;
        if (!world.GetBlock(pos.x, pos.y, pos.z, block) || !IsPuddle(world, pos, block)) continue;
        // At rest: not waiting for an update, and drawn at its true volume (the water
        // simulation snaps that once a cell settles).
        if (active.count(pos) || parked.count(pos) || block.shown != block.water) continue;
        SetWater(world, pos, block, 0);
        evaporatedCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void WaterSimulation::RainOn(ServerWorld& world, const glm::ivec3& hit) {
    // Up to the top of the water in this column.
    glm::ivec3 top = hit;
    Block block, b;
    if (!world.GetBlock(top.x, top.y, top.z, block) || !block.HasWater()) return;
    for (int i = 0; i < 256 && world.GetBlock(top.x, top.y + 1, top.z, b) && b.HasWater(); i++) {
        top.y++;
        block = b;
    }
    // Only an unfilled cell on top of more water: a body of water, not a puddle, and
    // never a new cell.
    const int room = block.WaterCapacity() - block.WaterVolume();
    if (room <= 0) return;
    if (!world.GetBlock(top.x, top.y - 1, top.z, b) || !b.HasWater()) return;
    // Open to the sky: nothing but air or plants above, as far as is loaded.
    for (int y = top.y + 1; y <= top.y + 64; y++) {
        if (!world.GetBlock(top.x, y, top.z, b)) break; // unloaded: assume open
        if (!b.IsAir() && !b.IsPlant()) return;
    }
    const int added = std::min(kRainUnits, room);
    SetWater(world, top, block, block.WaterVolume() + added);
    rainedUnits.fetch_add(static_cast<size_t>(added), std::memory_order_relaxed);
}

void WaterSimulation::Wake() {
    // Wake the thread if it's idle; cheap when it's already awake.
    if (thread.joinable() && !pending.exchange(true)) {
        std::lock_guard<std::mutex> lock(wakeMutex);
        wake.notify_one();
    }
}

bool WaterSimulation::NearCenter(const glm::ivec3& pos, int range) const {
    const glm::ivec3 chunk = Coords::ChunkOf(pos);
    for (const auto& center : centers) {
        const glm::ivec3 d = glm::abs(chunk - center);
        if (d.x <= range && d.y <= range && d.z <= range) return true;
    }
    return false;
}

void WaterSimulation::SetSimulationArea(const std::vector<glm::ivec3>& newCenters, int newDistance, int keepDistance) {
    limited = true;
    centers = newCenters;
    hasArea = !centers.empty();
    distance = newDistance;
    bool woke = false;
    for (auto it = parked.begin(); it != parked.end();) {
        if (NearCenter(*it, distance)) {
            active.insert(*it);
            woke = true;
        }
        else if (NearCenter(*it, keepDistance)) {
            ++it;
            continue;
        }
        it = parked.erase(it);
    }
    parkedCount.store(parked.size(), std::memory_order_relaxed);
    activeCount.store(active.size(), std::memory_order_relaxed);
    if (woke || hasArea) Wake(); // random ticks run whenever there's an area
}

void WaterSimulation::Start(ServerWorld& world) {
    if (thread.joinable()) return;
    stopping = false;
    thread = std::thread(&WaterSimulation::ThreadLoop, this, std::ref(world));
}

void WaterSimulation::Stop() {
    {
        std::lock_guard<std::mutex> lock(wakeMutex);
        stopping = true;
    }
    wake.notify_all();
    if (thread.joinable()) thread.join();
}

void WaterSimulation::ThreadLoop(ServerWorld& world) {
    using Clock = std::chrono::steady_clock;
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(kTickInterval));
    auto nextTick = Clock::now();
    while (true) {
        {
            std::unique_lock<std::mutex> lock(wakeMutex);
            // Idle until something changes; then keep a steady tick rate.
            // Puddles can only dry up while a player is around (random ticks run in
            // the simulation area), so with players connected it keeps ticking.
            wake.wait(lock, [&] { return stopping || pending.load() || GetActiveCount() > 0 || GetPumpCount() > 0 || hasArea.load(); });
            if (stopping) return;
            pending = false;
            if (wake.wait_until(lock, nextTick, [&] { return stopping; })) return;
        }
        nextTick = std::max(nextTick + interval, Clock::now());
        Tick(world);
    }
}

void WaterSimulation::Tick(ServerWorld& world) {
    std::recursive_mutex& edit = world.GetEditMutex();
    struct Ticking {
        std::atomic<bool>& flag;
        explicit Ticking(std::atomic<bool>& f) : flag(f) { flag.store(true, std::memory_order_release); }
        ~Ticking() { flag.store(false, std::memory_order_release); }
    } ticking(this->ticking);
    // Let waiting edits (the player's) go first before each hold of the edit mutex.
    auto yieldToEdits = [&]() { while (world.HasPendingEdits()) std::this_thread::yield(); };

    // Take this tick's batch out of the active set first: updating cells activates
    // more cells, which belong to the next tick (so water moves one step per tick).
    // Most woken cells are air or solid and are discarded with a single lookup, so
    // only water cells count against the budget (with a cap on the cheap ones too).
    // Pumps first: the water they move flows from this tick on. Then random ticks,
    // which dry up resting puddles.
    if (GetPumpCount() > 0) {
        yieldToEdits();
        std::lock_guard<std::recursive_mutex> lock(edit);
        RunPumps(world);
    }
    yieldToEdits();
    RandomTicks(world);

    batch.clear();
    std::vector<glm::ivec3> taken;
    size_t looked = 0;
    size_t wetInBatch = 0, dryInBatch = 0;
    while (wetInBatch < kCellsPerTick && looked < 8 * kCellsPerTick) {
        // Take a small chunk under the edit mutex (so it's held only briefly), then
        // filter it outside: block lookups are safe without the edit mutex.
        taken.clear();
        bool exhausted = false;
        {
            yieldToEdits();
            std::lock_guard<std::recursive_mutex> lock(edit);
            for (auto it = active.begin(); it != active.end() && taken.size() < kCellsPerSlice * 8;) {
                // Out of the simulation area: wait there until a player comes near.
                if (limited && !NearCenter(*it, distance)) parked.insert(*it);
                else taken.push_back(*it);
                it = active.erase(it);
            }
            activeCount.store(active.size(), std::memory_order_relaxed);
            parkedCount.store(parked.size(), std::memory_order_relaxed);
            exhausted = active.empty();
        }
        if (taken.empty()) {
            if (exhausted) break;
            continue; // everything taken was parked; look further
        }
        looked += taken.size();
        for (const auto& pos : taken) {
            Block block;
            if (!world.GetBlock(pos.x, pos.y, pos.z, block) || !block.CanHoldWater()) continue;
            // Cells that are empty now stay in the batch too: water can reach them earlier
            // in the same tick (an edge cell that just poured gets refilled from behind,
            // and can pour again this tick instead of every other one). They're cheap, so
            // only cells holding water count against the budget.
            if (!block.HasWater()) {
                if (dryInBatch++ < 8 * kCellsPerTick) batch.push_back({ pos, false, block.WaterFloor() });
                continue;
            }
            // Falling cells (room below) sort ahead of others at the same height.
            Block below;
            const bool falling = !block.IsPartial() && world.GetBlock(pos.x, pos.y - 1, pos.z, below) &&
                                 below.CanHoldWater() && below.WaterVolume() < below.WaterCapacity();
            batch.push_back({ pos, falling, block.WaterLevel() });
            wetInBatch++;
        }
    }
    // Bottom up, falling cells first at each height: lower water moves out of the way
    // before the water above it moves, so a falling stream stays continuous and an
    // edge cell empties before the water behind it refills it.
    // Within a height, higher water goes first: water then passes through a cell in
    // the same tick it arrives (upstream gives before downstream takes), so a draining
    // surface goes steadily down instead of jittering up and down.
    std::sort(batch.begin(), batch.end(), [](const Pending& a, const Pending& b) {
        if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
        if (a.falling != b.falling) return a.falling > b.falling;
        if (a.level != b.level) return a.level > b.level;
        // Ties in a fixed order rather than hash order, so it's stable tick to tick.
        if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
        return a.pos.z < b.pos.z;
    });

    // Update in slices, releasing the edit mutex in between so player edits (and the
    // main thread generally) never wait long.
    for (size_t start = 0; start < batch.size(); start += kCellsPerSlice) {
        yieldToEdits();
        std::lock_guard<std::recursive_mutex> lock(edit);
        const size_t end = std::min(batch.size(), start + kCellsPerSlice);
        for (size_t i = start; i < end; i++) UpdateCell(world, batch[i].pos);
    }
}

void WaterSimulation::UpdateCell(ServerWorld& world, const glm::ivec3& pos) {
    Block block;
    if (!world.GetBlock(pos.x, pos.y, pos.z, block) || !block.HasWater()) return;
    int volume = block.WaterVolume();

    // --- 1. Fall into whatever room the cell below has ---
    // A partial block's bottom is solid, so water in it can't fall out.
    if (!block.IsPartial()) {
        const glm::ivec3 down = pos + kDown;
        Block below;
        if (world.GetBlock(down.x, down.y, down.z, below) && below.CanHoldWater()) {
            const int room = below.WaterCapacity() - below.WaterVolume();
            if (room > 0) {
                const int moved = std::min(volume, room);
                SetWater(world, down, below, below.WaterVolume() + moved);
                volume -= moved;
                SetWater(world, pos, block, volume);
                if (volume == 0) return;
                block = block.WithWater(static_cast<uint8_t>(volume));
            }
        }
    }

    // --- 2. Level out with lower water nearby ---
    // Compares water levels (floor + volume), so water fills the space above sloped
    // blocks. A cell with room below it (over a drop) counts as that much lower: water
    // pours into it in one big share, and it falls as a whole on its next update, so
    // water going over an edge falls as a solid stream rather than thin slices.
    // Looks along each horizontal direction up to kReach cells, continuing through
    // water; dry cells are only flowed into when directly adjacent.
    struct Member {
        glm::ivec3 pos;
        Block cell;
        int drop; // room below it (a cell over a drop counts as that much lower)
    };
    auto roomBelow = [&](const glm::ivec3& p, const Block& cell) {
        if (cell.IsPartial()) return 0; // solid bottom
        Block b;
        if (!world.GetBlock(p.x, p.y - 1, p.z, b) || !b.CanHoldWater()) return 0;
        return static_cast<int>(b.WaterCapacity()) - b.WaterVolume();
    };
    auto member = [&](const glm::ivec3& p, const Block& cell) { return Member{ p, cell, roomBelow(p, cell) }; };
    // The water a member holds below level t, and the level its water stands at. A
    // partial block fills its low parts first (Block::WaterVolumeAt).
    auto volumeAt = [](const Member& m, int t) { return m.cell.WaterVolumeAt(t + m.drop); };
    auto levelOf = [](const Member& m, int volume) { return m.cell.WaterLevelFor(volume) - m.drop; };

    Member group[1 + 4 * kReach];
    int count = 0;
    group[count++] = member(pos, block);
    const int level = levelOf(group[0], volume);
    // Thin water only flows onto dry ground with enough head, or when deep water right
    // behind it keeps it fed (a lake pouring over a low slope); a puddle edge stays put.
    bool backed = volume >= kMinSpreadVolume;
    for (const auto& d : kHorizontal) {
        if (backed) break;
        const glm::ivec3 np = pos + d;
        Block n;
        if (world.GetBlock(np.x, np.y, np.z, n) && n.WaterVolume() >= kMinSpreadVolume && n.WaterLevel() >= block.WaterLevel()) backed = true;
    }
    // Where the water in a partial block came in from: the sides with water beside
    // them. It only reaches the open quarters joined to those (two notches meeting at a
    // corner don't make a channel), unless it came from above (no wet sides).
    bool wetSide[4] = {};
    bool anyWetSide = false;
    if (block.IsPartial())
        for (int i = 0; i < 4; i++) {
            const glm::ivec3 np = pos + kHorizontal[i];
            Block n;
            wetSide[i] = world.GetBlock(np.x, np.y, np.z, n) && n.HasWater() && OpenFace(block, kHorizontal[i], level);
            anyWetSide |= wetSide[i];
        }
    for (int i = 0; i < 4; i++) {
        const glm::ivec3& d = kHorizontal[i];
        // Out of this cell toward d only through an open quarter on that side...
        if (!OpenFace(block, d, level)) continue;
        // ...that the water in it can reach.
        if (anyWetSide && !wetSide[i]) {
            bool reachable = false;
            for (int j = 0; j < 4 && !reachable; j++) reachable = wetSide[j] && PassesThrough(block, kHorizontal[j], d, level);
            if (!reachable) continue;
        }
        Block previous = block;
        for (int step = 1; step <= kReach; step++) {
            const glm::ivec3 np = pos + d * step;
            Block n;
            if (!world.GetBlock(np.x, np.y, np.z, n) || !n.CanHoldWater()) break; // unloaded or solid: a wall
            // Through a partial block only where it's open below the water: in by the
            // near side, and (going on past it) right through to the far side.
            if (!OpenFace(n, -d, level)) break;
            if (step > 1 && !PassesThrough(previous, -d, d, level)) break;
            previous = n;
            const Member m = member(np, n);
            const int head = level - levelOf(m, n.WaterVolume());
            if (!n.HasWater()) {
                if (step == 1 && head >= 2 && (backed || head >= kMinSpreadVolume)) group[count++] = m;
                break;
            }
            if (head >= 2) group[count++] = m;
        }
    }
    if (count == 1) {
        SnapShown(world, pos, block);
        return;
    }

    // Find the common level T where the group's water fits: sum over members of
    // clamp(T - floor, 0, capacity) == total. Then hand out the remainder one unit at
    // a time (this cell first) to members that can still take more at level T.
    int total = 0;
    for (int i = 0; i < count; i++) total += group[i].cell.WaterVolume();
    auto share = [&](const Member& m, int t) { return volumeAt(m, t); };
    auto filled = [&](int t) {
        int sum = 0;
        for (int i = 0; i < count; i++) sum += share(group[i], t);
        return sum;
    };
    int lo = -static_cast<int>(Block::kMaxWaterVolume), hi = Block::kMaxWaterVolume;
    while (lo < hi) { // largest T with filled(T) <= total
        const int mid = lo + (hi - lo + 1) / 2;
        if (filled(mid) <= total) lo = mid; else hi = mid - 1;
    }
    int remainder = total - filled(lo);
    int target[1 + 4 * kReach];
    for (int i = 0; i < count; i++) {
        const Member& m = group[i];
        target[i] = share(m, lo);
        if (remainder > 0 && target[i] < m.cell.WaterCapacity() && volumeAt(m, lo + 1) > target[i]) { target[i]++; remainder--; }
    }

    bool moved = false;
    for (int i = 0; i < count; i++) {
        if (target[i] == group[i].cell.WaterVolume()) continue;
        SetWater(world, group[i].pos, group[i].cell, target[i]);
        moved = true;
    }
    if (!moved) SnapShown(world, pos, block);
}
