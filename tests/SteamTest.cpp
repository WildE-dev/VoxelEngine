// Lava (world/Lava.h) and steam (server/Steam.h). Checks that
//  - steam rises and spreads out from where it's let off, thins as it goes, and fades
//    away; it keeps out of solid blocks and water, and a block put where it is pushes
//    it up;
//  - lava runs kReach blocks out over the level from a source, pours down over an edge,
//    and drains away once its source is gone;
//  - water touching lava boils away into a big cloud of steam, and the lava stays;
//  - over a real connection: a creative player pours water beside lava they've put
//    down, the server boils it away, and the steam reaches the client; a survival
//    player can't put lava down.
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <tuple>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "server/Steam.h"
#include "world/Fluids.h"
#include "world/Lava.h"
#include "world/Signals.h"
#include "world/TerrainGenerator.h"

constexpr int kTestViewDistance = 4;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

// A stone floor under y = 0, air above, and whatever's set.
struct Cells : BlockAccess {
    std::map<std::tuple<int, int, int>, Block> blocks;
    bool GetBlock(int x, int y, int z, Block& block) const override {
        auto it = blocks.find({ x, y, z });
        block = it != blocks.end() ? it->second : (y < 0 ? Block(BlockType::STONE) : Block());
        return true;
    }
    Block At(int x, int y, int z) const {
        Block b;
        GetBlock(x, y, z, b);
        return b;
    }
    void Apply(const std::vector<Lava::Change>& changes) {
        for (const auto& c : changes) blocks[{ c.pos.x, c.pos.y, c.pos.z }] = c.block;
    }
    // Steps the lava cells (all of them, each time) until nothing changes.
    void RunLava(std::vector<Lava::Boil>* boils = nullptr, int steps = 40) {
        for (int i = 0; i < steps; i++) {
            std::vector<glm::ivec3> lava;
            for (const auto& [k, b] : blocks)
                if (b.IsLava()) lava.emplace_back(std::get<0>(k), std::get<1>(k), std::get<2>(k));
            std::vector<Lava::Change> changes;
            std::vector<Lava::Boil> made;
            Lava::Step(*this, lava, changes, made);
            if (boils) boils->insert(boils->end(), made.begin(), made.end());
            if (changes.empty()) return;
            Apply(changes);
        }
    }
};

struct TestClient {
    ClientWorld world;
    Client client{ world };
    glm::vec3 feet{ 0.0f };
    bool joined = false;
    void Update() {
        client.Poll();
        if (!joined && client.IsWelcomed()) joined = true, feet = client.GetSpawn();
        if (auto c = client.TakeCorrection()) feet = *c;
        if (!joined) return;
        world.Update(feet + glm::vec3(0.0f, 1.62f, 0.0f));
        client.SendPlayerState(feet, glm::vec3(0.0f), 0.0f, 0.0f, net::kFlagOnGround);
    }
};

bool Pump(TestClient& c, double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        c.Update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

} // namespace

int main() {
    std::cout << "Steam" << std::endl;
    {
        Cells world;
        SteamCloud steam;
        steam.Add({ 0, 2, 0 }, Block::kMaxWaterVolume * Lava::kSteamPerUnit); // (a block of water's worth)
        const long long start = steam.Total();
        for (int i = 0; i < 6; i++) steam.Update(world);
        int highest = 0, widest = 0;
        for (const auto& [cell, units] : steam.All()) highest = std::max(highest, cell.y), widest = std::max(widest, std::abs(cell.x));
        Check(steam.All().size() > 20 && highest >= 5 && widest >= 2 && steam.At({ 0, 2, 0 }) < 400,
              "a burst rises and spreads out (" + std::to_string(steam.All().size()) + " cells, up to y " + std::to_string(highest) + ")");
        Check(steam.Total() < start && steam.Total() > start / 2, "and fades as it goes (" + std::to_string(steam.Total()) + " of " + std::to_string(start) + ")");
        bool clean = true;
        for (const auto& [cell, units] : steam.All()) clean &= cell.y >= 0;
        Check(clean, "it keeps out of the ground");
        for (int i = 0; i < 240; i++) steam.Update(world); // (a minute)
        Check(steam.All().empty(), "and is gone in under a minute");

        // Under a roof it spreads sideways; a block put where it is pushes it up.
        Cells roofed;
        for (int x = -6; x <= 6; x++)
            for (int z = -6; z <= 6; z++) roofed.blocks[{ x, 3, z }] = Block(BlockType::STONE);
        SteamCloud under;
        under.Add({ 0, 1, 0 }, 1500);
        for (int i = 0; i < 20; i++) under.Update(roofed);
        bool belowRoof = true;
        for (const auto& [cell, units] : under.All()) belowRoof &= cell.y < 3;
        Check(belowRoof && under.At({ 2, 2, 0 }) > 0, "under a roof it spreads out beneath it");
        SteamCloud pushed;
        pushed.Add({ 0, 1, 0 }, 100);
        Cells filled;
        filled.blocks[{ 0, 1, 0 }] = Block(BlockType::STONE);
        pushed.Update(filled);
        Check(pushed.At({ 0, 1, 0 }) == 0 && pushed.At({ 0, 2, 0 }) > 0, "and a block put where it is pushes it up");
        Cells wet;
        wet.blocks[{ 1, 0, 0 }] = Block::Water(Block::kMaxWaterVolume);
        SteamCloud dry;
        dry.Add({ 0, 0, 0 }, 1000);
        for (int i = 0; i < 5; i++) dry.Update(wet);
        Check(dry.At({ 1, 0, 0 }) == 0, "it doesn't go into water");
    }

    std::cout << "Lava" << std::endl;
    {
        auto total = [](const Cells& c) {
            int n = 0;
            for (const auto& [k, b] : c.blocks) n += Lava::Volume(b);
            return n;
        };
        auto deepest = [](const Cells& c) {
            int n = 0;
            for (const auto& [k, b] : c.blocks) n = std::max(n, Lava::Volume(b));
            return n;
        };
        // A block of lava on the level spreads out like water, slowly, into a small pool: never
        // made or lost, and no thinner than a quarter of a block (or so) at its edge.
        Cells world;
        world.blocks[{ 0, 0, 0 }] = Lava::WithVolume(Lava::kFull);
        world.RunLava(nullptr, 200);
        int cells = 0, thinnest = Lava::kFull;
        for (const auto& [k, b] : world.blocks)
            if (b.IsLava()) cells++, thinnest = std::min(thinnest, Lava::Volume(b));
        Check(total(world) == Lava::kFull && cells >= 3 && cells <= 9 && deepest(world) < Lava::kFull,
              "a block of lava spreads into a small pool, all of it kept (" + std::to_string(cells) + " cells)");
        // Into a pit: it falls in and fills it from the bottom.
        Cells pit;
        for (int x = -1; x <= 1; x++)
            for (int z = -1; z <= 1; z++)
                if (x || z) pit.blocks[{ x, -1, z }] = Block(BlockType::STONE), pit.blocks[{ x, -2, z }] = Block(BlockType::STONE);
        pit.blocks[{ 0, -1, 0 }] = Block();
        pit.blocks[{ 0, -2, 0 }] = Block();
        pit.blocks[{ 0, 2, 0 }] = Lava::WithVolume(Lava::kFull);
        pit.RunLava(nullptr, 40);
        Check(Lava::Volume(pit.At(0, -2, 0)) == Lava::kFull && !pit.At(0, 2, 0).IsLava() && total(pit) == Lava::kFull,
              "it falls, as water does, and fills a pit from the bottom");
        // Over an edge: off a ledge it pours down to the ground below.
        Cells ledge;
        for (int x = -3; x <= 0; x++)
            for (int y = 0; y <= 2; y++) ledge.blocks[{ x, y, 0 }] = Block(BlockType::STONE);
        ledge.blocks[{ 0, 3, 0 }] = Lava::WithVolume(Lava::kFull);
        ledge.blocks[{ -1, 3, 0 }] = Lava::WithVolume(Lava::kFull);
        ledge.RunLava(nullptr, 200);
        int low = 0;
        for (const auto& [k, b] : ledge.blocks)
            if (std::get<1>(k) == 0) low += Lava::Volume(b);
        Check(low > 0 && total(ledge) == 2 * Lava::kFull, "it pours over an edge to the ground below");

        // Water beside it boils away into steam; the lava stays.
        Cells pool;
        pool.blocks[{ 0, 0, 0 }] = Lava::WithVolume(Lava::kFull);
        pool.blocks[{ 0, 0, 1 }] = Block(BlockType::STONE);
        pool.blocks[{ 1, 0, 0 }] = Block::Water(Block::kMaxWaterVolume);
        std::vector<Lava::Boil> boils;
        pool.RunLava(&boils, 1);
        int made = 0;
        for (const auto& b : boils) made += b.steam;
        Check(!pool.At(1, 0, 0).HasWater() && pool.At(0, 0, 0).type == BlockType::OBSIDIAN && made == Block::kMaxWaterVolume * Lava::kSteamPerUnit,
              "a block of water touching it boils away into " + std::to_string(made) + " units of steam, and cools it to obsidian");
        Cells trickle;
        trickle.blocks[{ 0, 0, 0 }] = Lava::WithVolume(Lava::kFull);
        trickle.blocks[{ 0, 0, 1 }] = Block(BlockType::STONE);
        trickle.blocks[{ 1, 0, 0 }] = Block::Water(16);
        trickle.RunLava(nullptr, 1);
        Check(!trickle.At(1, 0, 0).HasWater() && trickle.At(0, 0, 0).IsLava(), "a trickle just boils away, leaving the lava as it is");
    }

    std::cout << "Over the network" << std::endl;
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.worldType = TerrainGenerator::Type::Empty; // grass at row -1
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Creative;
    options.creatures = false;
    Server server;
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();
    TestClient a;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Smith", error, kTestViewDistance);
    auto at = [&](const glm::ivec3& p) {
        Block b;
        return a.world.GetBlock(p.x, p.y, p.z, b) ? b : Block(BlockType::COUNT);
    };
    Check(Pump(a, 20.0, [&] { return a.joined && at({ 10, -1, 10 }).type == BlockType::GRASS; }), "a creative player joins");
    server.QueueConsoleCommand("tp Smith 7.5 0 7.5");
    Pump(a, 5.0, [&] { return std::abs(a.feet.x - 7.5f) < 0.1f; });
    // A pit of stone: lava in one end, and a wall between it and where the water goes.
    server.QueueConsoleCommand("fill 10 -3 10 14 -1 12 stone");
    Pump(a, 10.0, [&] { return at({ 14, -1, 12 }).type == BlockType::STONE; });
    a.client.BreakBlock({ 11, -1, 11 });
    a.client.BreakBlock({ 12, -1, 11 });
    Check(Pump(a, 10.0, [&] { return at({ 11, -1, 11 }).IsAir() && at({ 12, -1, 11 }).IsAir(); }), "the player digs a little trench");
    Check(a.client.PlaceBlock({ 11, -1, 11 }, BlockType::LAVA) && Pump(a, 10.0, [&] { return at({ 11, -1, 11 }).IsLava(); }),
          "puts lava in one end of it");
    Check(Pump(a, 10.0, [&] { return at({ 12, -1, 11 }).IsLava(); }), "and it runs along the trench");
    server.QueueConsoleCommand("fill 13 -1 11 13 -1 11 water");
    Check(Pump(a, 10.0, [&] { return !a.client.GetSteam().empty(); }), "water poured beside the lava boils away, and the steam reaches the client");
    Check(Pump(a, 5.0, [&] { return !at({ 13, -1, 11 }).HasWater() && at({ 12, -1, 11 }).type == BlockType::OBSIDIAN; }),
          "the water's gone, and the lava it touched has cooled to obsidian");
    size_t biggest = 0;
    const auto boiled = std::chrono::steady_clock::now();
    const bool faded = Pump(a, 90.0, [&] {
        biggest = std::max(biggest, a.client.GetSteam().size());
        return a.client.GetSteam().empty();
    });
    const double lasted = std::chrono::duration<double>(std::chrono::steady_clock::now() - boiled).count();
    Check(faded && biggest > 60 && lasted > 5.0, "a big cloud (" + std::to_string(biggest) + " cells at most), faded away after " + std::to_string(static_cast<int>(lasted)) + " s");

    // Steam in pipes lets itself out: out of an open end's end face, and out of an outlet.
    auto steamAt = [&](const glm::ivec3& cell) {
        for (const auto& s : a.client.GetSteam())
            if (s.cell == cell) return true;
        return false;
    };
    server.QueueConsoleCommand("fill 16 0 14 18 0 14 fluid_pipe");
    server.QueueConsoleCommand("fill 18 0 13 18 0 13 stone"); // (beside an end: not where it comes out)
    Pump(a, 10.0, [&] { return at({ 18, 0, 14 }).type == BlockType::FLUID_PIPE; });
    server.QueueConsoleCommand("fluid 17 0 14 steam");
    bool left = false, right = false;
    Pump(a, 10.0, [&] {
        for (const auto& s : a.client.GetSteam()) {
            if (s.cell.x <= 15) left = true;
            if (s.cell.x >= 19) right = true;
        }
        return left && right;
    });
    std::string seen;
    for (const auto& s : a.client.GetSteam()) seen += " (" + std::to_string(s.cell.x) + "," + std::to_string(s.cell.y) + "," + std::to_string(s.cell.z) + ")";
    Check(left && right, "steam in a pipe lets itself out of both open ends" + (left && right ? std::string() : seen));
    Check(Pump(a, 20.0, [&] { return a.client.GetSteam().empty(); }), "until it's all out (and the cloud fades)");
    // Ends closed (stone in front of each), but an outlet on the pipe facing up.
    server.QueueConsoleCommand("fill 15 0 14 15 0 14 stone");
    server.QueueConsoleCommand("fill 19 0 14 19 0 14 stone");
    server.QueueConsoleCommand("fill 17 0 15 17 0 15 fluid_outlet");
    Pump(a, 10.0, [&] { return at({ 17, 0, 15 }).type == BlockType::FLUID_OUTLET; });
    server.QueueConsoleCommand("fluid 17 0 14 steam");
    Check(Pump(a, 10.0, [&] { return steamAt({ 18, 0, 15 }); }) && !steamAt({ 19, 1, 14 }) && !steamAt({ 15, 1, 14 }) ,
          "with its ends closed, it comes out of an outlet on the pipe (out of its front)");
    Pump(a, 20.0, [&] { return a.client.GetSteam().empty(); });
    // A shut fluid valve keeps the two sides apart: steam on one side doesn't reach the
    // other side's open end; opened, it does.
    server.QueueConsoleCommand("fill 16 0 20 20 0 20 fluid_pipe");
    server.QueueConsoleCommand("fill 18 0 20 18 0 20 fluid_valve");
    server.QueueConsoleCommand("fill 15 0 20 15 0 20 stone");
    Pump(a, 10.0, [&] { return at({ 18, 0, 20 }).type == BlockType::FLUID_VALVE && at({ 20, 0, 20 }).type == BlockType::FLUID_PIPE; });
    server.QueueConsoleCommand("fluid 16 0 20 steam");
    Check(!Pump(a, 4.0, [&] { return steamAt({ 21, 0, 20 }); }), "a shut fluid valve keeps steam from the far side");
    server.QueueConsoleCommand("tp Smith 18.5 0 17.5"); // (within reach of it)
    Pump(a, 5.0, [&] { return std::abs(a.feet.z - 17.5f) < 0.1f; });
    a.client.UseBed({ 18, 0, 20 });
    Check(Pump(a, 5.0, [&] { return at({ 18, 0, 20 }).type == BlockType::FLUID_VALVE_OPEN; }), "a fluid valve opens by hand");
    server.QueueConsoleCommand("fluid 16 0 20 steam");
    Check(Pump(a, 10.0, [&] { return steamAt({ 21, 0, 20 }); }), "and open, the steam gets through to the far end");

    // Shutting a valve keeps what was in it: a tank of water, a pipe, an open valve, a
    // pipe and an empty tank; shut, the two sides hold all of it between them.
    {
        auto waterIn = [&](const glm::ivec3& p) {
            // (The newest answer: the list is capped, so it's told by when it came.)
            const double before = a.client.GetInspections().empty() ? -1.0 : a.client.GetInspections().back().time;
            a.client.Inspect(p);
            int amount = -1;
            Pump(a, 5.0, [&] {
                const auto& lines = a.client.GetInspections();
                if (lines.empty() || lines.back().time <= before) return false;
                const std::string& t = lines.back().text;
                const size_t w = t.find("water ");
                amount = w == std::string::npos ? 0 : std::atoi(t.c_str() + w + 6);
                return true;
            });
            return amount;
        };
        server.QueueConsoleCommand("tp Smith 18.5 0 22.5");
        Pump(a, 5.0, [&] { return std::abs(a.feet.z - 22.5f) < 0.1f; });
        server.QueueConsoleCommand("fill 16 0 25 20 0 25 fluid_pipe");
        server.QueueConsoleCommand("fill 16 0 25 16 0 25 fluid_tank");
        server.QueueConsoleCommand("fill 20 0 25 20 0 25 fluid_tank");
        server.QueueConsoleCommand("fill 18 0 25 18 0 25 fluid_valve_open");
        Pump(a, 10.0, [&] { return at({ 18, 0, 25 }).type == BlockType::FLUID_VALVE_OPEN && at({ 20, 0, 25 }).type == BlockType::FLUID_TANK; });
        server.QueueConsoleCommand("fluid 16 0 25 water");
        Check(Pump(a, 10.0, [&] { return waterIn({ 20, 0, 25 }) > 0; }), "water in a tank spreads through an open valve to the far tank");
        Pump(a, 1.0, [] { return false; }); // (a power update or two: its share is in the valve too)
        a.client.UseBed({ 18, 0, 25 });
        Check(Pump(a, 5.0, [&] { return at({ 18, 0, 25 }).type == BlockType::FLUID_VALVE; }), "the valve shuts by hand");
        Pump(a, 1.0, [] { return false; });
        const int near = waterIn({ 16, 0, 25 }), far = waterIn({ 20, 0, 25 });
        Check(near + far == Fluids::kPerTank, "and the two sides keep all of it (" + std::to_string(near) + " + " + std::to_string(far) + " of " + std::to_string(Fluids::kPerTank) + ")");
    }

    // Obsidian takes a blast: TNT between it and stone, lit.
    server.QueueConsoleCommand("tp Smith 26.5 0 26.5");
    Pump(a, 5.0, [&] { return std::abs(a.feet.x - 26.5f) < 0.1f; });
    server.QueueConsoleCommand("fill 30 0 30 30 0 30 tnt");
    server.QueueConsoleCommand("fill 31 0 30 31 0 30 obsidian");
    server.QueueConsoleCommand("fill 29 0 30 29 0 30 stone");
    server.QueueConsoleCommand("fill 30 0 31 30 0 31 reinforced_concrete");
    Pump(a, 5.0, [&] { return at({ 31, 0, 30 }).type == BlockType::OBSIDIAN && at({ 30, 0, 30 }).type == BlockType::TNT; });
    a.client.UseBed({ 30, 0, 30 });
    Check(Pump(a, 8.0, [&] { return at({ 29, 0, 30 }).IsAir() && at({ 30, 0, 30 }).IsAir(); }) && at({ 31, 0, 30 }).type == BlockType::OBSIDIAN,
          "TNT blows the stone away, but the obsidian stands");
    Check(at({ 30, 0, 31 }).type == BlockType::REINFORCED_CONCRETE, "and so does reinforced concrete");

    // A boiler fired by lava: obsidian pipes from a pool of lava to its side, water under
    // it and a tank for steam over it, and no coal.
    server.QueueConsoleCommand("fill 40 -1 40 46 -1 44 stone");
    server.QueueConsoleCommand("fill 40 -1 42 40 -1 42 air");
    server.QueueConsoleCommand("fill 40 -1 42 40 -1 42 lava");
    server.QueueConsoleCommand("fill 41 -1 42 42 -1 42 obsidian_pipe");
    server.QueueConsoleCommand("fill 43 -1 42 43 -1 42 boiler");
    server.QueueConsoleCommand("fill 43 -2 42 43 -2 42 fluid_tank");
    server.QueueConsoleCommand("fill 43 0 42 43 0 42 fluid_tank");
    Pump(a, 10.0, [&] { return at({ 43, 0, 42 }).type == BlockType::FLUID_TANK && at({ 41, -1, 42 }).type == BlockType::OBSIDIAN_PIPE; });
    server.QueueConsoleCommand("fluid 43 -2 42 water");
    auto steamTank = [&] {
        for (const auto& m : a.client.GetMachines())
            if (m.pos == glm::ivec3(43, 0, 42)) return m.depth == static_cast<uint8_t>(Fluids::Kind::Steam) && m.pressure > 0;
        return false;
    };
    Check(Pump(a, 15.0, steamTank), "obsidian pipes dipped in lava fire a boiler (no coal): it boils water into steam");

    server.QueueConsoleCommand("gamemode survival Smith");
    Pump(a, 10.0, [&] { return !a.client.IsCreative(); });
    Check(!a.client.PlaceBlock({ 10, 0, 10 }, BlockType::LAVA) && !a.client.PlaceBlock({ 10, 0, 10 }, BlockType::AIR),
          "a survival player can't put lava down (and an empty hand places nothing)");
    // Standing in the trench beside the lava when water comes in: the steam scalds.
    server.QueueConsoleCommand("fill 12 -1 11 12 -1 11 lava");
    Pump(a, 5.0, [&] { return at({ 12, -1, 11 }).IsLava(); });
    server.QueueConsoleCommand("tp Smith 13.5 -1 11.5");
    Pump(a, 5.0, [&] { return std::abs(a.feet.y + 1.0f) < 0.1f; });
    Pump(a, 1.5, [] { return false; }); // (past being just teleported)
    const int healthBefore = a.client.GetHealth();
    server.QueueConsoleCommand("fill 13 -1 11 13 -1 11 water");
    const bool scalded = Pump(a, 10.0, [&] { return a.client.GetHealth() < healthBefore; });
    Check(scalded, "thick steam scalds a survival player in it (" + std::to_string(healthBefore) + " -> " + std::to_string(a.client.GetHealth()) + " half hearts)");
    // A mask keeps it out: the same again, masked, and no scalding.
    server.QueueConsoleCommand("give mask 1 Smith");
    Pump(a, 10.0, [&] { return (a.client.GetArmour() & Armour::Bit(ArmourPiece::Mask)) != 0; });
    Pump(a, 6.0, [] { return false; }); // (the first cloud thins away)
    server.QueueConsoleCommand("fill 13 -1 11 13 -1 11 air");
    server.QueueConsoleCommand("fill 12 -1 11 12 -1 11 lava");
    Pump(a, 5.0, [&] { return at({ 12, -1, 11 }).IsLava(); });
    int lowest = a.client.GetHealth();
    const int maskedBefore = lowest;
    server.QueueConsoleCommand("fill 13 -1 11 13 -1 11 water");
    Pump(a, 6.0, [&] { lowest = std::min(lowest, a.client.GetHealth()); return false; });
    Check(lowest >= maskedBefore, "with a mask on, the steam doesn't scald (" + std::to_string(maskedBefore) + " -> lowest " + std::to_string(lowest) + ")");

    a.client.Disconnect();
    Pump(a, 0.3, [] { return false; });
    server.Stop();
    std::cout << (failures ? "STEAM TEST FAILED (" : "ALL STEAM CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
