// Water simulation distance: water only flows near players. Steps the water simulation
// directly (no thread, no network) and checks that
//  - water within the simulation distance of a player falls,
//  - water beyond it (but loaded) waits where it is, parked, with its volume intact,
//  - it carries on once the player moves close enough,
//  - parked water in chunks that unload is forgotten.
#include <chrono>
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
