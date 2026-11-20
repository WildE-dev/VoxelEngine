#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <random>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

#include "world/Block.h"
#include "world/Coords.h"

class ServerWorld;

// Finite, volume-conserving water. Each cell holds 0..Block::kMaxWaterVolume units:
// water blocks, and partial (sloped) blocks whose open space above the slope can be
// waterlogged. Water is only ever moved, never created or destroyed.
//
// Each update of a cell:
//  1. Fall: move as much as fits into the cell below (all of it into empty space).
//     A cell with room below it counts as that much lower when levelling (step 2),
//     so water pours over edges in big shares and falls as a stream, not slices.
//  2. Level out by water level (a partial block counts as a cell with a raised floor
//     at its slope's average height): share with water up to 6 cells away along each
//     horizontal direction (through connected water) whose level is at least 2 units
//     lower, so the group ends up level. Water flows into dry cells directly beside
//     it if it stands at least 1/8 of a block above them, is that deep itself, or is
//     fed by deep water right behind it.
//
// With 128 units per cell and levelling over 6-cell spans, resting water slopes by at
// most 1/768 of a block per cell, so lakes stay flat when tapped. The threshold and the
// minimum depth are what make water settle: spilled water ends as a shallow puddle.
// Every step either moves water down or evens out a group of cells, so the system
// always comes to rest.
//
// Event driven: only cells next to something that changed are simulated, so streamed
// in seas and lakes cost nothing until disturbed. Unloaded space acts as a wall.
//
// Evaporation, the one exception to water only ever moving, runs on random ticks:
// every tick, kRandomTicksPerChunk random cells of each chunk in the simulation area
// are looked at, and a shallow puddle at rest among them dries up. A puddle is a cell
// with at most kPuddleVolume units that isn't full, has no water above or below it
// (a lake's thin top layer isn't a puddle) and isn't waiting for an update (so it's
// not flowing). Each cell is picked about every
// 4096 / kRandomTicksPerChunk ticks, so a spill fades away cell by cell over a few
// minutes. Lakes are deep, so only thin water left by spills and drained pools goes.
//
// Rain (while SetRaining is on) also runs on the random ticks: when one lands in a body
// of water, the column's top water cell gets kRainUnits more, if it's open to the sky,
// isn't full and has water under it. Rain never puts water in a dry cell or above a
// full one, so it tops drained lakes and pools back up to the level they had, and
// deep water (hit more often) fills fastest. Puddles don't evaporate while it rains.
//
// Pumps: each tick, a pump takes up to kPumpRate units from the water in the cells
// around it and sends them through the pipes connected to it (pipes join face to
// face), out of the network's open ends: the open cells beside a pipe that joins only
// one other pipe or pump. Ends get equal shares. A pump only takes what the ends have
// room for, so water is still only ever moved; it can lift water any height. Pumps
// run within the simulation area like flowing water.
//
// The server limits simulation to an area around the players (SetSimulationArea),
// smaller than the area it loads: active cells outside it are parked, untouched, and
// carry on from where they were once a player comes close enough again.
// In the game it runs on its own thread (Start); tests step it with Tick.
class WaterSimulation
{
public:
    ~WaterSimulation() { Stop(); }

    // Marks a changed cell and its neighbours for the next tick. Called by
    // ServerWorld::SetBlock with the world's edit mutex held.
    void Activate(const glm::ivec3& pos);

    // Runs ticks on a dedicated thread (every kTickInterval while there's work, idle
    // otherwise) until Stop. Without it, call Tick directly.
    void Start(ServerWorld& world);
    void Stop();

    // Runs exactly one tick now: every currently active cell (up to kCellsPerTick
    // holding water) gets one update. Any thread; takes the world's edit mutex in
    // slices so other edits can interleave.
    void Tick(ServerWorld& world);

    // Only cells within `distance` chunks (per axis) of one of these chunk coordinates
    // are simulated from now on; the rest are parked until they're in range again.
    // Parked cells further than `keepDistance` from every centre are forgotten (their
    // chunks are about to unload; saved water is woken again when it loads). Until
    // this is first called, everything is simulated. Caller holds the world's edit
    // mutex (ServerWorld::Update).
    void SetSimulationArea(const std::vector<glm::ivec3>& centers, int distance, int keepDistance);

    // A pump was placed / removed at this position. Called by ServerWorld::SetBlock
    // (and when a saved chunk with pumps loads) with the world's edit mutex held.
    void AddPump(const glm::ivec3& pos);
    void RemovePump(const glm::ivec3& pos);
    size_t GetPumpCount() const { return pumpCount.load(std::memory_order_relaxed); }

    size_t GetActiveCount() const { return activeCount.load(std::memory_order_relaxed); }
    // Rain on or off (the server's weather). Any thread.
    void SetRaining(bool on) { raining.store(on, std::memory_order_relaxed); }
    bool IsRaining() const { return raining.load(std::memory_order_relaxed); }
    // Units rain has added so far (for tests and stats).
    size_t GetRainedUnits() const { return rainedUnits.load(std::memory_order_relaxed); }

    // Puddles dried up so far (for tests and stats).
    size_t GetEvaporatedCount() const { return evaporatedCount.load(std::memory_order_relaxed); }
    size_t GetParkedCount() const { return parkedCount.load(std::memory_order_relaxed); }
    // Nothing queued and no tick in progress.
    bool IsIdle() const { return GetActiveCount() == 0 && !ticking.load(std::memory_order_acquire); }

    static constexpr float kTickInterval = 0.1f;
    static constexpr size_t kCellsPerTick = 32768;
    // Cells updated per hold of the edit mutex; bounds how long a player edit waits.
    static constexpr size_t kCellsPerSlice = 128;
    // Units a pump moves per tick: a quarter of a block, 2.5 blocks a second.
    static constexpr int kPumpRate = Block::kMaxWaterVolume / 4;
    // Puddles: up to a quarter of a block deep. With 3 random cells per chunk per tick
    // a puddle cell lasts about 1365 ticks (2.3 minutes) on average.
    static constexpr int kPuddleVolume = Block::kMaxWaterVolume / 4;
    static constexpr int kRandomTicksPerChunk = 3;
    // Water a raindrop hit adds to a surface cell: a 16th of a block.
    static constexpr int kRainUnits = Block::kMaxWaterVolume / 16;
    // Pipes searched per pump per tick; larger networks only reach this many.
    static constexpr size_t kMaxPipes = 4096;

private:
    void UpdateCell(ServerWorld& world, const glm::ivec3& pos);
    // Runs every pump in the simulation area once. Edit mutex held.
    void RunPumps(ServerWorld& world);
    // Random ticks over the simulation area: dries up the resting puddles they find.
    // Takes the edit mutex itself.
    void RandomTicks(ServerWorld& world);
    static bool IsPuddle(const ServerWorld& world, const glm::ivec3& pos, const Block& block);
    // Rain landing in the body of water at `pos`. Edit mutex held.
    void RainOn(ServerWorld& world, const glm::ivec3& pos);
    void RunPump(ServerWorld& world, const glm::ivec3& pump);
    // Whether a cell is within `range` chunks of a simulation centre. Edit mutex held.
    bool NearCenter(const glm::ivec3& pos, int range) const;
    void Wake();
    void ThreadLoop(ServerWorld& world);

    // Guarded by the world's edit mutex.
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> active;
    std::atomic<size_t> activeCount{0};
    // Simulation area, guarded by the world's edit mutex like `active`.
    bool limited = false;
    std::atomic<bool> hasArea{false}; // limited, with at least one centre
    std::vector<glm::ivec3> centers;
    int distance = 0;
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> parked;
    std::atomic<size_t> parkedCount{0};
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> pumps; // guarded by the edit mutex
    std::atomic<size_t> pumpCount{0};

    std::mt19937 random{0x5EED}; // ticking thread only (fixed seed: tests repeat)
    std::vector<glm::ivec3> randomChunks, puddles, wet; // tick scratch
    std::atomic<bool> raining{false};
    std::atomic<size_t> rainedUnits{0};
    std::atomic<size_t> evaporatedCount{0};
    std::atomic<bool> ticking{false};
    struct Pending {
        glm::ivec3 pos;
        bool falling;
        int level;
    };
    std::vector<Pending> batch; // tick-local (one ticking thread at a time)

    std::thread thread;
    std::mutex wakeMutex;
    std::condition_variable wake;
    bool stopping = false;
    std::atomic<bool> pending{false}; // set by Activate, so the thread wakes from idle
};
