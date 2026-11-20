#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#include <glm/glm.hpp>

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

    size_t GetActiveCount() const { return activeCount.load(std::memory_order_relaxed); }
    // Nothing queued and no tick in progress.
    bool IsIdle() const { return GetActiveCount() == 0 && !ticking.load(std::memory_order_acquire); }

    static constexpr float kTickInterval = 0.1f;
    static constexpr size_t kCellsPerTick = 32768;
    // Cells updated per hold of the edit mutex; bounds how long a player edit waits.
    static constexpr size_t kCellsPerSlice = 128;

private:
    void UpdateCell(ServerWorld& world, const glm::ivec3& pos);
    void ThreadLoop(ServerWorld& world);

    // Guarded by the world's edit mutex.
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> active;
    std::atomic<size_t> activeCount{0};
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
