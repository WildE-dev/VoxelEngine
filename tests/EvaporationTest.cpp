// Empty worlds and puddle evaporation, stepping the water simulation directly.
// Checks that
//  - an empty world is flat grass over dirt over stone everywhere, with nothing on it,
//  - a server remembers the world type in the save and keeps it on reopening,
//  - water spilled on flat ground spreads into a shallow puddle that dries up cell by
//    cell over time (random ticks), not all at once,
//  - deep water (a walled pool) doesn't evaporate, and nothing does without a player
//    nearby.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include "server/Server.h"
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
        for (int x = -1; x <= 2 && all; x++)
            for (int y = -1; y <= 0 && all; y++)
                for (int z = -1; z <= 2 && all; z++) {
                    auto c = world.FindChunk({ x, y, z });
                    all = c && c->IsLoaded() && c->GetLightVersion() > 0;
                }
        if (all) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// Water in the open area around (8, 0, 8) on the ground row.
struct Puddle {
    int cells = 0, total = 0, deepest = 0;
};
Puddle Measure(const ServerWorld& world) {
    Puddle p;
    for (int x = -8; x <= 24; x++)
        for (int z = -8; z <= 24; z++) {
            Block b;
            if (world.GetBlock(x, 0, z, b) && b.HasWater()) {
                p.cells++;
                p.total += b.WaterVolume();
                p.deepest = std::max(p.deepest, static_cast<int>(b.WaterVolume()));
            }
        }
    return p;
}

} // namespace

int main() {
    constexpr int S = Chunk::CHUNK_SIZE;

    std::cout << "Empty world" << std::endl;
    {
        TerrainGenerator empty(TerrainGenerator::Type::Empty);
        auto blocks = std::make_unique<TerrainGenerator::Blocks>();
        bool flat = true, bare = true;
        for (const glm::ivec3 chunk : { glm::ivec3(0, -1, 0), glm::ivec3(63, -1, -17), glm::ivec3(-40, -1, 9) }) {
            TerrainGenerator::ColumnHeights heights;
            empty.ComputeColumnHeights(chunk.x, chunk.z, heights);
            empty.GenerateChunk(chunk, heights, *blocks);
            for (int x = 0; x < S; x++)
                for (int z = 0; z < S; z++) {
                    // Chunk y -1 is rows -16..-1: grass on top, dirt, then stone.
                    flat &= (*blocks)[Chunk::Index(x, 15, z)].type == BlockType::GRASS;
                    flat &= (*blocks)[Chunk::Index(x, 12, z)].type == BlockType::DIRT;
                    flat &= (*blocks)[Chunk::Index(x, 0, z)].type == BlockType::STONE;
                }
            empty.ComputeColumnHeights(chunk.x, chunk.z, heights);
            empty.GenerateChunk(chunk + glm::ivec3(0, 1, 0), heights, *blocks);
            for (const Block& b : *blocks) bare &= b.IsAir();
        }
        Check(flat, "the ground is grass at row -1 over dirt over stone, the same everywhere");
        Check(bare, "and there's nothing above it (no trees, plants, rocks or water)");

        std::random_device rd;
        const auto dir = std::filesystem::temp_directory_path() / ("voxel-empty-test-" + std::to_string(rd()));
        auto run = [&](TerrainGenerator::Type type) {
            Server server;
            Server::Options options;
            options.bindHost = "127.0.0.1";
            options.port = 0;
            options.log = false;
            options.worldDirectory = dir.string();
            options.worldType = type;
            if (!server.Start(options)) return false;
            server.RunInBackground();
            server.Stop();
            return true;
        };
        Check(run(TerrainGenerator::Type::Empty), "a server creates an empty world");
        {
            WorldStorage storage(dir);
            Check(storage.Open() && storage.GetWorldType() == static_cast<uint8_t>(TerrainGenerator::Type::Empty), "the save records it as empty");
        }
        Check(run(TerrainGenerator::Type::Normal), "reopening it without asking for an empty world works");
        {
            WorldStorage storage(dir);
            Check(storage.Open() && storage.GetWorldType() == static_cast<uint8_t>(TerrainGenerator::Type::Empty), "and it stays empty");
        }
        std::filesystem::remove_all(dir);
    }

    std::cout << "Evaporation" << std::endl;
    {
        TerrainGenerator terrain(TerrainGenerator::Type::Empty);
        ServerWorld world(terrain);
        world.SetWaterSimulationDistance(2);
        world.Update({ glm::ivec3(0) });
        Check(WaitLoaded(world), "chunks load");
        auto& sim = world.GetWater();
        auto tickUntilStill = [&] {
            for (int i = 0; i < 2000 && (i < 5 || sim.GetActiveCount() > 0); i++) sim.Tick(world);
        };

        // A walled pool of full water, 3x3 and 2 deep, sunk into the ground.
        for (int x = 19; x <= 23; x++)
            for (int z = 19; z <= 23; z++) {
                world.SetBlock(x, -3, z, BlockType::STONE);
                for (int y = -2; y <= 0; y++)
                    if (x == 19 || x == 23 || z == 19 || z == 23) world.SetBlock(x, y, z, BlockType::STONE);
            }
        for (int x = 20; x <= 22; x++)
            for (int z = 20; z <= 22; z++)
                for (int y = -2; y <= -1; y++) world.SetBlock(x, y, z, Block::Water(Block::kMaxWaterVolume));

        // One full block spilled on flat ground.
        world.SetBlock(8, 0, 8, Block::Water(Block::kMaxWaterVolume));
        tickUntilStill();
        const Puddle settled = Measure(world);
        Check(settled.cells > 4 && settled.total == Block::kMaxWaterVolume && settled.deepest <= WaterSimulation::kPuddleVolume,
              "the spill settles as a shallow puddle (" + std::to_string(settled.cells) + " cells, at most " +
                  std::to_string(settled.deepest) + " units deep)");

        // Without a player nearby nothing dries up.
        world.Update({ glm::ivec3(8, 0, 0) }); // loaded, but outside the simulation distance
        for (int i = 0; i < 3000; i++) sim.Tick(world);
        Check(Measure(world).cells == settled.cells, "with no player near, it stays");
        world.Update({ glm::ivec3(0) });

        for (int i = 0; i < 400; i++) sim.Tick(world);
        const Puddle early = Measure(world);
        Check(early.cells > 0 && early.cells < settled.cells,
              "it dries up gradually: " + std::to_string(early.cells) + " of " + std::to_string(settled.cells) + " cells left after 40 s");
        for (int i = 0; i < 20000 && Measure(world).cells > 0; i++) sim.Tick(world);
        Check(Measure(world).cells == 0, "and is gone eventually (" + std::to_string(sim.GetEvaporatedCount()) + " cells evaporated)");

        int pool = 0;
        for (int x = 20; x <= 22; x++)
            for (int z = 20; z <= 22; z++)
                for (int y = -2; y <= -1; y++) {
                    Block b;
                    world.GetBlock(x, y, z, b);
                    pool += b.WaterVolume();
                }
        Check(pool == 18 * Block::kMaxWaterVolume, "the deep pool hasn't lost any water");
    }

    std::cout << (failures ? "EVAPORATION TEST FAILED (" : "ALL EVAPORATION CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
