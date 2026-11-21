// Water simulation distance: water only flows near players. Steps the water simulation
// directly (no thread, no network) and checks that
//  - water within the simulation distance of a player falls,
//  - water beyond it (but loaded) waits where it is, parked, with its volume intact,
//  - it carries on once the player moves close enough,
//  - parked water in chunks that unload is forgotten,
//  - water flows into a carved notch lower than the block's average height (stepped
//    and sloped), filling the lowest part first.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "server/ServerWorld.h"

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

bool WaitFor(double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return done();
}

bool Loaded(const ServerWorld& world, const glm::ivec3& chunk) {
    auto c = world.FindChunk(chunk);
    return c && c->IsLoaded() && c->GetLightVersion() > 0;
}

int WaterAt(const ServerWorld& world, const glm::ivec3& p) {
    Block b;
    return world.GetBlock(p.x, p.y, p.z, b) ? b.WaterVolume() : -1;
}

// Total water in a column, from y down to the bottom of loaded space near the terrain.
int ColumnWater(const ServerWorld& world, const glm::ivec3& top) {
    int total = 0;
    for (int y = top.y; y >= -40; y--) total += std::max(0, WaterAt(world, { top.x, y, top.z }));
    return total;
}

} // namespace

int main() {
    constexpr int kDistance = 3;
    TerrainGenerator terrain;
    ServerWorld world(terrain);
    world.SetWaterSimulationDistance(kDistance);
    Check(world.GetWaterSimulationDistance() == kDistance, "the distance is set");
    {
        ServerWorld other(terrain);
        other.SetWaterSimulationDistance(100);
        Check(other.GetWaterSimulationDistance() < ServerWorld::RENDER_DISTANCE, "it's kept below the view distance");
    }

    // Air high above the terrain (chunk y 2 is rows 32..47): one cell near the player
    // (chunk x 0), one six chunks away, loaded but outside the simulation distance.
    const glm::ivec3 nearPos(8, 40, 8), farPos(6 * 16 + 8, 40, 8);
    world.Update({ glm::ivec3(0) });
    Check(WaitFor(60.0, [&] {
              for (int y = -2; y <= 2; y++)
                  if (!Loaded(world, { 0, y, 0 }) || !Loaded(world, { 6, y, 0 })) return false;
              return true;
          }),
          "chunks near and far load");

    const Block water = Block::Water(Block::kMaxWaterVolume);
    world.SetBlock(nearPos.x, nearPos.y, nearPos.z, water);
    world.SetBlock(farPos.x, farPos.y, farPos.z, water);
    const int farTotal = ColumnWater(world, farPos);

    auto& sim = world.GetWater();
    for (int i = 0; i < 30; i++) sim.Tick(world);
    Check(WaterAt(world, nearPos) == 0, "water near the player falls");
    Check(WaterAt(world, farPos) == Block::kMaxWaterVolume, "water outside the simulation distance stays put");
    Check(sim.GetParkedCount() > 0, "it's parked (" + std::to_string(sim.GetParkedCount()) + " cells)");
    Check(ColumnWater(world, farPos) == farTotal, "with its volume intact");

    // Carved blocks: a walled trough, half a block of water next to a block carved
    // down in one place. The block's average height is above the water, but the notch
    // reaches the floor: the water must flow in.
    {
        const Block wall(BlockType::STONE_BRICK);
        auto trough = [&](int z, const Block& carved) {
            for (int x = 0; x <= 5; x++)
                for (int dz = -1; dz <= 1; dz++) {
                    world.SetBlock(x, 43, z + dz, wall);                    // floor
                    if (dz != 0 || x == 0 || x >= 3) world.SetBlock(x, 44, z + dz, wall); // walls
                }
            world.SetBlock(1, 44, z, Block::Water(Block::kMaxWaterVolume / 2));
            world.SetBlock(2, 44, z, carved);
        };
        const Block notched = Block(BlockType::STONE_BRICK).Chiselled(3, -8); // one quarter cut to the floor
        Block slope(BlockType::DIRT);
        slope = slope.Chiselled(0, -8);                                        // one corner of a slope cut down
        Check(notched.WaterFloor() > Block::kMaxWaterVolume / 2 && slope.WaterFloor() > Block::kMaxWaterVolume / 2,
              "the carved blocks' average heights are above half a block");
        Check(notched.WaterLevelFor(0) == 0 && notched.WaterVolumeAt(64) == 16 && notched.WaterLevelFor(16) >= 64 && notched.WaterLevelFor(16) < 68 && notched.WaterLevelFor(notched.WaterCapacity()) == 128 &&
                  notched.WaterVolumeAt(128) == notched.WaterCapacity(),
              "a notched block fills its notch first (half a block deep: 16 units, a quarter of the cell)");
        trough(4, notched);
        trough(8, slope);
        for (int i = 0; i < 60; i++) sim.Tick(world);
        Block a, b;
        world.GetBlock(1, 44, 4, a);
        world.GetBlock(2, 44, 4, b);
        Check(b.WaterVolume() > 0 && std::abs(a.WaterLevel() - b.WaterLevel()) <= 2 && a.WaterVolume() + b.WaterVolume() == 64,
              "water flows into a stepped block's notch and levels with the pool (" + std::to_string(a.WaterLevel()) + " / " +
                  std::to_string(b.WaterLevel()) + ", " + std::to_string(b.WaterVolume()) + " units in the notch)");
        world.GetBlock(1, 44, 8, a);
        world.GetBlock(2, 44, 8, b);
        Check(b.WaterVolume() > 0 && std::abs(a.WaterLevel() - b.WaterLevel()) <= 3 && a.WaterVolume() + b.WaterVolume() == 64,
              "and into a slope's lowered corner (" + std::to_string(a.WaterLevel()) + " / " + std::to_string(b.WaterLevel()) + ")");
    }

    // The player walks over to it.
    world.Update({ glm::ivec3(5, 0, 0) });
    for (int i = 0; i < 30; i++) sim.Tick(world);
    Check(WaterAt(world, farPos) == 0, "it falls once the player is close enough");
    Check(ColumnWater(world, farPos) == farTotal, "and no water is lost on the way down");

    // Water far out, then everyone leaves: it isn't remembered for unloaded chunks.
    world.SetBlock(nearPos.x, nearPos.y + 2, nearPos.z, water);
    world.Update({ glm::ivec3(40, 0, 0) });
    for (int i = 0; i < 3; i++) sim.Tick(world);
    world.Update({ glm::ivec3(41, 0, 0) });
    Check(sim.GetParkedCount() == 0, "parked water in unloaded chunks is forgotten");

    std::cout << (failures ? "WATER AREA TEST FAILED (" : "ALL WATER AREA CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
