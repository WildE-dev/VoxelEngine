// Pumps and pipes, stepping the water simulation directly (no thread, no network).
// High above the terrain, a stone cup of water sits beside a pump; a pipe climbs from
// the pump and ends over a second, empty stone cup. Checks that
//  - nothing moves until the pump has a pipe,
//  - the pump moves kPumpRate units a tick, lifting the water up the pipe,
//  - the water is conserved: the source drains exactly as the destination fills,
//  - it stops when the destination is full (taking nothing it can't place),
//  - disconnecting the pipe or breaking the pump stops it, and placing it again
//    restarts it,
//  - a pump outside the simulation distance waits,
//  - a saved pump keeps pumping after its chunk is saved and loaded again,
//  - a spring adds kSpringRate units a tick to the open cells around it while
//    they have room, and stops when it's broken; a drain takes kDrainRate.
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <thread>

#include "server/ServerWorld.h"
#include "server/WorldStorage.h"

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

bool WaitLoaded(ServerWorld& world) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        bool all = true;
        for (int y = 1; y <= 4 && all; y++) {
            auto c = world.FindChunk({ 0, y, 0 });
            all = c && c->IsLoaded() && c->GetLightVersion() > 0;
        }
        if (all) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int WaterAt(const ServerWorld& world, const glm::ivec3& p) {
    Block b;
    return world.GetBlock(p.x, p.y, p.z, b) ? b.WaterVolume() : -1;
}

void Set(ServerWorld& world, const glm::ivec3& p, const Block& b) {
    world.SetBlock(p.x, p.y, p.z, b);
}

// Layout (all in chunk 0,2..3,0, high above the terrain):
const glm::ivec3 kSource(5, 40, 5);   // cup of water
const glm::ivec3 kPump(6, 40, 5);     // pump beside it
const int kPipeTop = 46;              // pipe climbs from (6,41,5) to (6,46,5), then one step +x
const glm::ivec3 kPipeEnd(7, 46, 5);
const glm::ivec3 kDest(8, 46, 5);     // empty cup at the pipe's end

// A one-cell stone cup open only at the top and toward `open`.
void Cup(ServerWorld& world, const glm::ivec3& cell, const glm::ivec3& open) {
    const glm::ivec3 around[5] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 0, -1, 0 } };
    for (const auto& d : around)
        if (cell + d != open) Set(world, cell + d, Block(BlockType::STONE));
}

void Build(ServerWorld& world, bool withPipe) {
    Cup(world, kSource, kPump);
    Set(world, kSource, Block::Water(Block::kMaxWaterVolume));
    // Stone around the pump, apart from the source and the pipe.
    for (const auto& d : { glm::ivec3(1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1), glm::ivec3(0, -1, 0) })
        Set(world, kPump + d, Block(BlockType::STONE));
    Set(world, kPump, Block(BlockType::PUMP));
    // Stone around the pipe end, apart from the destination.
    for (const auto& d : { glm::ivec3(0, 1, 0), glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1) })
        Set(world, kPipeEnd + d, Block(BlockType::STONE));
    Cup(world, kDest, kPipeEnd);
    if (!withPipe) return;
    for (int y = kPump.y + 1; y <= kPipeTop; y++) Set(world, { kPump.x, y, kPump.z }, Block(BlockType::PIPE));
    Set(world, kPipeEnd, Block(BlockType::PIPE));
}

// Pumps run on air (world/Power.h); the server grants it. Here every tick gives the
// pump all it could want, unless `powered` is off.
bool powered = true;
void Tick(ServerWorld& world, int n) {
    for (int i = 0; i < n; i++) {
        {
            std::lock_guard<std::recursive_mutex> lock(world.GetEditMutex());
            world.GetWater().SetPumpBudget(kPump, powered ? 1e9f : 0.0f);
        }
        world.GetWater().Tick(world);
    }
}

} // namespace

int main() {
    constexpr int kFull = Block::kMaxWaterVolume, kRate = WaterSimulation::kPumpRate;
    // An empty world: no dungeon pump rooms to register pumps of their own meanwhile.
    TerrainGenerator terrain(TerrainGenerator::Type::Empty);

    std::cout << "Pumping" << std::endl;
    {
        ServerWorld world(terrain);
        world.Update({ glm::ivec3(0) });
        Check(WaitLoaded(world), "chunks load");

        Build(world, false);
        Check(world.GetWater().GetPumpCount() == 1, "the pump is registered");
        Tick(world, 5);
        Check(WaterAt(world, kSource) == kFull, "a pump without pipes moves nothing");

        for (int y = kPump.y + 1; y <= kPipeTop; y++) Set(world, { kPump.x, y, kPump.z }, Block(BlockType::PIPE));
        Set(world, kPipeEnd, Block(BlockType::PIPE));
        powered = false;
        Tick(world, 3);
        WaterSimulation::PumpUse use;
        {
            std::lock_guard<std::recursive_mutex> lock(world.GetEditMutex());
            use = world.GetWater().TakePumpUse(kPump);
        }
        Check(WaterAt(world, kSource) == kFull && use.wanted && use.lifted == 0.0f, "without air it moves nothing (though it has work to do)");
        // Air for just 10 units lifted the 6 blocks up to the destination: 10 go.
        const int lift = kDest.y - kPump.y;
        {
            std::lock_guard<std::recursive_mutex> lock(world.GetEditMutex());
            world.GetWater().SetPumpBudget(kPump, 10.0f * lift);
        }
        world.GetWater().Tick(world);
        {
            std::lock_guard<std::recursive_mutex> lock(world.GetEditMutex());
            use = world.GetWater().TakePumpUse(kPump);
        }
        const int moved = kFull - WaterAt(world, kSource);
        Check(moved == 10 && use.lifted == 10.0f * lift, "air for 10 units lifted 6 blocks moves just those (" + std::to_string(moved) + " units, " +
                                                             std::to_string(static_cast<int>(use.lifted)) + " unit-blocks)");
        // Put it back as it was, then power it fully.
        Set(world, kSource, Block::Water(kFull));
        Set(world, kDest, Block::Water(0));
        powered = true;
        Tick(world, 1);
        Check(WaterAt(world, kSource) == kFull - kRate && WaterAt(world, kDest) == kRate,
              "with a pipe it moves " + std::to_string(kRate) + " units a tick, up 6 blocks (source " +
                  std::to_string(WaterAt(world, kSource)) + ", destination " + std::to_string(WaterAt(world, kDest)) + ")");

        const glm::ivec3 firstPipe = kPump + glm::ivec3(0, 1, 0);
        Set(world, firstPipe, Block(BlockType::STONE));
        Tick(world, 2);
        Check(WaterAt(world, kSource) == kFull - kRate, "disconnecting the pipe stops it");
        Set(world, firstPipe, Block(BlockType::PIPE));

        Tick(world, 10);
        Check(WaterAt(world, kSource) == 0 && WaterAt(world, kDest) == kFull, "the source drains into the destination, nothing lost");

        // Refill the source with the destination full: nothing may move.
        Set(world, kSource, Block::Water(kFull));
        Tick(world, 3);
        Check(WaterAt(world, kSource) == kFull && WaterAt(world, kDest) == kFull, "it stops when the destination is full");

        // Empty the destination, remove the pump: nothing moves; put it back: it runs.
        Set(world, kDest, Block());
        Set(world, kPump, Block(BlockType::STONE));
        Check(world.GetWater().GetPumpCount() == 0, "a broken pump is unregistered");
        Tick(world, 2);
        Check(WaterAt(world, kSource) == kFull, "and moves nothing");
        Set(world, kPump, Block(BlockType::PUMP));
        Tick(world, 1);
        Check(WaterAt(world, kDest) == kRate, "placing it again starts it");

        // Out of the simulation area it waits.
        world.SetWaterSimulationDistance(2);
        world.Update({ glm::ivec3(6, 0, 0) });
        const int before = WaterAt(world, kDest);
        Tick(world, 3);
        Check(WaterAt(world, kDest) == before, "a pump outside the simulation distance waits");
        world.Update({ glm::ivec3(1, 0, 0) });
        Tick(world, 1);
        Check(WaterAt(world, kDest) == std::min(before + kRate, kFull), "and resumes when a player comes close");
    }

    std::cout << "Springs" << std::endl;
    {
        // A spring with stone round its sides, under a one-cell stone cup.
        constexpr int kSpring = WaterSimulation::kSpringRate;
        ServerWorld world(terrain);
        world.Update({ glm::ivec3(0) });
        Check(WaitLoaded(world), "chunks load");
        const glm::ivec3 spring(20, 40, 20), above = spring + glm::ivec3(0, 1, 0);
        for (const auto& d : { glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1), glm::ivec3(0, -1, 0) })
            Set(world, spring + d, Block(BlockType::STONE));
        Cup(world, above, spring);
        const size_t pumpsBefore = world.GetWater().GetPumpCount();
        Set(world, spring, Block(BlockType::SPRING));
        Tick(world, 1);
        Check(world.GetWater().GetPumpCount() == pumpsBefore + 1, "a spring is registered (with the pumps)");
        Check(WaterAt(world, above) == kSpring, "it adds " + std::to_string(kSpring) + " units a tick to the cell above (" + std::to_string(WaterAt(world, above)) + ")");
        Tick(world, 6);
        Check(WaterAt(world, above) == kFull && WaterAt(world, above + glm::ivec3(0, 1, 0)) == 0, "until the cup is full, then stops (water doesn't climb)");
        Set(world, above + glm::ivec3(1, 0, 0), Block());
        Tick(world, 6);
        int total = 0;
        for (int x = 15; x <= 25; x++)
            for (int y = 30; y <= 42; y++)
                for (int z = 15; z <= 25; z++) total += std::max(0, WaterAt(world, { x, y, z }));
        Check(total > kFull + 3 * kSpring, "opening the cup lets it run out, and the spring keeps it coming (" + std::to_string(total) + " units about)");
        Set(world, spring, Block(BlockType::STONE));
        Tick(world, 1);
        Check(world.GetWater().GetPumpCount() == pumpsBefore, "breaking it unregisters it");

        // A drain under a cup of water.
        constexpr int kDrain = WaterSimulation::kDrainRate;
        const glm::ivec3 drain(30, 40, 20), cup = drain + glm::ivec3(0, 1, 0);
        for (const auto& d : { glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1), glm::ivec3(0, -1, 0) })
            Set(world, drain + d, Block(BlockType::STONE));
        Cup(world, cup, drain);
        Set(world, cup, Block::Water(kFull));
        Set(world, drain, Block(BlockType::DRAIN));
        Tick(world, 1);
        Check(world.GetWater().GetPumpCount() == pumpsBefore + 1 && WaterAt(world, cup) == kFull - kDrain,
              "a drain takes " + std::to_string(kDrain) + " units a tick from the cell above (" + std::to_string(WaterAt(world, cup)) + " left)");
        Tick(world, 2);
        Check(WaterAt(world, cup) == 0, "until it's empty");
    }

    std::cout << "Saved pumps" << std::endl;
    {
        std::random_device rd;
        const auto dir = std::filesystem::temp_directory_path() / ("voxel-pump-test-" + std::to_string(rd()));
        {
            WorldStorage storage(dir);
            Check(storage.Open(), "world opens");
            ServerWorld world(terrain, &storage);
            world.Update({ glm::ivec3(0) });
            Check(WaitLoaded(world), "chunks load");
            Build(world, true);
            Tick(world, 1);
            world.SaveAll();
            Check(storage.Flush(), "saved after one tick");
        }
        {
            WorldStorage storage(dir);
            Check(storage.Open(), "world reopens");
            ServerWorld world(terrain, &storage);
            world.Update({ glm::ivec3(0) });
            Check(WaitLoaded(world), "chunks load");
            Check(world.GetWater().GetPumpCount() == 1, "the saved pump is registered again");
            Tick(world, 1);
            Check(WaterAt(world, kDest) == std::min(2 * kRate, kFull), "and carries on pumping (as far as the source and the room allow)");
        }
        std::filesystem::remove_all(dir);
    }

    std::cout << (failures ? "PUMP TEST FAILED (" : "ALL PUMP CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
