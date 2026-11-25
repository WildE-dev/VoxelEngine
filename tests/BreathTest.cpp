// Breathing (survival). Checks the rules (world/Breathing.h): air drains under water,
// is capped high up and deep down (lower the further), and comes back; and over
// a real connection, with a client that stays wherever the server puts it, that
//  - on the ground the air stays full,
//  - with the head under water it drains, the player drowns once it's gone (and the
//    death is announced), then respawns with full air,
//  - far up (above 300) and deep underground (below -100) it settles at a cap that
//    falls with the distance, and hurts only where there's none,
//  - it comes back on the ground,
//  - in creative nothing takes it.
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "TestSections.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Bodies.h"
#include "world/Breathing.h"
#include "world/Crafting.h"
#include "world/Armour.h"

// Test clients ask for a short view (chunks): plenty for the checks, and light enough
// to run every suite at once.
constexpr int kTestViewDistance = 4;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

// A client that stands still wherever it was put (no physics: it can hang in the air
// or stand in rock), so its breathing depends only on where that is.
struct HoldingClient {
    ClientWorld world;
    Client client{ world };
    glm::vec3 feet{ 0.0f };
    bool joined = false;

    void Update() {
        client.Poll();
        if (!joined && client.IsWelcomed()) {
            joined = true;
            feet = client.GetSpawn();
        }
        if (auto c = client.TakeCorrection()) feet = *c;
        if (!joined) return;
        world.Update(feet + glm::vec3(0.0f, Bodies::PlayerBody().eyeHeight, 0.0f));
        client.SendPlayerState(feet, glm::vec3(0.0f), 0.0f, 0.0f, net::kFlagOnGround);
    }
};

bool Pump(std::initializer_list<HoldingClient*> clients, double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto* c : clients) c->Update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

bool Has(const HoldingClient& c, const std::string& text) {
    for (const auto& line : c.client.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    TestSections sections(argc, argv);
    using namespace Breathing;
    std::cout << "Rules" << std::endl;
    Check(At(10.0f, false).cause == Cause::None && At(10.0f, false).rate > 0.0f, "on the surface the air comes back");
    Check(At(10.0f, true).cause == Cause::Water && At(10.0f, true).rate * kDrownSeconds == -kMaxAir, "under water a breath lasts 15 seconds");
    Check(At(290.0f, false).cause == Cause::None && At(-90.0f, false).cause == Cause::None && At(-52.0f, false).cause == Cause::None,
          "mountain tops, caves and dungeon floors (-52) are fine");
    Check(At(10.0f, false).limit == kMaxAir && At(290.0f, false).limit == kMaxAir, "elsewhere there's a full breath to take");
    Check(At(350.0f, false).cause == Cause::ThinAir && At(350.0f, false).limit == kMaxAir / 2 && At(400.0f, false).limit == 0.0f &&
              At(450.0f, false).limit == 0.0f && At(350.0f, false).rate > 0.0f,
          "thin air above 300 caps the air (half at 350, none from 400) without draining it");
    Check(At(-150.0f, false).cause == Cause::StaleAir && At(-150.0f, false).limit == kMaxAir / 2 && At(-200.0f, false).limit == 0.0f,
          "stale air below -100 the same way down (half at -150, none at -200)");
    {
        // The meter settles at the cap and stays there; under water it drains through it.
        float a = kMaxAir;
        for (int i = 0; i < 200; i++) a = Step(a, At(350.0f, false), 0.05f);
        const float settled = a;
        for (int i = 0; i < 200; i++) a = Step(a, At(350.0f, false), 0.05f);
        float b = settled;
        for (int i = 0; i < 20; i++) b = Step(b, At(350.0f, true), 0.05f);
        Check(settled == kMaxAir / 2 && a == settled && b < settled && !Suffocating(settled, At(350.0f, false)) && Suffocating(0.0f, At(400.0f, false)),
              "the meter settles at the cap, drains under water, and only hurts with nothing to breathe");
    }
    Block half = Block::Water(Block::kMaxWaterVolume / 2);
    Check(UnderWater(half, 0.3f) && !UnderWater(half, 0.7f) && !UnderWater(Block(), 0.1f), "a point is under water below the water's surface");

    Server server;
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = false;
    options.worldType = TerrainGenerator::Type::Empty; // flat ground: the top of row -1 is at 0
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival;
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();

    HoldingClient a, builder;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Diver", error, kTestViewDistance);
    builder.client.Connect("127.0.0.1", server.GetPort(), "Builder", error, kTestViewDistance);
    auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &builder }, s, done); };
    Check(both(20.0, [&] { return a.joined && builder.joined; }), "two players join");
    server.QueueConsoleCommand("gamemode creative Builder");
    server.QueueConsoleCommand("tp Diver 8.5 0 8.5");
    both(20.0, [&] { return builder.client.IsCreative(); });
    both(2.0, [] { return false; });
    Check(a.client.GetAir() == a.client.GetMaxAir() && a.client.GetAirCause() == Cause::None, "on the ground the air stays full");

    if (sections.Run("water")) {
        std::cout << "Water" << std::endl;
        // A two-deep, one-wide well, full of water: it can't flow anywhere.
        const glm::ivec3 well(20, -1, 20);
        builder.client.BreakBlock(well);
        builder.client.BreakBlock(well + glm::ivec3(0, -1, 0));
        auto at = [&](const glm::ivec3& p) { Block b; return a.world.GetBlock(p.x, p.y, p.z, b) ? b : Block(BlockType::COUNT); };
        both(20.0, [&] { return at(well).IsAir() && at(well + glm::ivec3(0, -1, 0)).IsAir(); });
        builder.client.PlaceBlock(well + glm::ivec3(0, -1, 0), BlockType::WATER);
        builder.client.PlaceBlock(well, BlockType::WATER);
        both(20.0, [&] { return at(well).HasWater() && at(well + glm::ivec3(0, -1, 0)).HasWater(); });
        server.QueueConsoleCommand("tp Diver 20.5 -2 20.5"); // eyes at -0.38: in the top cell's water
        Check(both(20.0, [&] { return a.client.GetAirCause() == Cause::Water && a.client.GetAir() < a.client.GetMaxAir(); }), "head under water: the air drains");
        const auto t0 = std::chrono::steady_clock::now();
        Check(both(20.0, [&] { return a.client.GetAir() == 0; }), "and runs out");
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Check(took > 10.0 && took < 19.0, "after about 15 seconds (" + std::to_string(took) + ")");
        Check(both(20.0, [&] { return a.client.GetHealth() < 20; }), "then drowning hurts");
        Check(both(15.0, [&] { return Has(builder, "Diver drowned"); }), "until the diver drowns, which everyone hears about");
        Check(both(20.0, [&] { return a.client.GetAir() == a.client.GetMaxAir() && a.client.GetHealth() == 20; }), "and respawns with full air and health");

    }
    if (sections.Run("air")) {
        std::cout << "Thin and stale air" << std::endl;
        server.QueueConsoleCommand("tp Diver 8.5 345 8.5"); // eyes at 346.6
        Check(both(20.0, [&] { return a.client.GetAirCause() == Cause::ThinAir && a.client.GetAir() == 11; }), "high up, the air thins to its cap (10.7 at 346.6)");
        both(4.0, [] { return false; });
        Check(a.client.GetAir() == 11 && a.client.GetHealth() == 20, "and stays there, without hurting");
        server.QueueConsoleCommand("tp Diver 8.5 400 8.5");
        Check(both(20.0, [&] { return a.client.GetAir() == 0; }) && both(20.0, [&] { return a.client.GetHealth() < 20; }), "above 400 there's none, and that hurts");
        server.QueueConsoleCommand("tp Diver 8.5 0 8.5");
        Check(both(20.0, [&] { return a.client.GetAir() == a.client.GetMaxAir() && a.client.GetAirCause() == Cause::None; }), "back on the ground, breath comes back");
        server.QueueConsoleCommand("tp Diver 8.5 -95 8.5"); // eyes at -93.4: not yet
        both(2.0, [] { return false; });
        Check(a.client.GetAirCause() == Cause::None && a.client.GetAir() == a.client.GetMaxAir(), "at -95 the air is still fine");
        server.QueueConsoleCommand("tp Diver 8.5 -130 8.5");
        Check(both(20.0, [&] { return a.client.GetAirCause() == Cause::StaleAir && a.client.GetAir() == 15; }), "deep underground the air is stale (capped at 14.3 at -128.4)");

        std::cout << "Air diffuser" << std::endl;
        // A diffuser beside the diver, an air pipe on it, the pipe filled with air.
        server.QueueConsoleCommand("fill 10 -129 8 10 -129 8 air_diffuser");
        server.QueueConsoleCommand("fill 11 -129 8 12 -129 8 air_pipe");
        Check(both(10.0, [&] { return a.client.GetAirCause() == Cause::StaleAir; }), "with the diffuser built but no air in its pipes, it's still stale");
        bool fresh = false;
        for (int i = 0; i < 10 && !fresh; i++) {
            server.QueueConsoleCommand("pressurise 12 -129 8");
            fresh = both(1.0, [&] { return a.client.GetAirCause() == Cause::None && a.client.GetAir() == a.client.GetMaxAir(); });
        }
        Check(fresh, "with air pressure it breathes out fresh air: a full breath again, deep underground");
        server.QueueConsoleCommand("tp Diver 30.5 -130 8.5"); // twenty blocks off
        Check(both(20.0, [&] { return a.client.GetAirCause() == Cause::StaleAir; }), "out of its reach, the air is stale again");
        server.QueueConsoleCommand("tp Diver 8.5 -130 8.5");

    }
    if (sections.Run("water")) {
        std::cout << "Oxygen tank" << std::endl;
        server.QueueConsoleCommand("give iron_ingot 3 Diver");
        server.QueueConsoleCommand("give glass 1 Diver");
        both(10.0, [&] { return a.client.GetResources(BlockType::GLASS) >= Block::kUnitsPerBlock && a.client.GetResources(ItemType::IRON_INGOT) >= 3 * Block::kUnitsPerBlock; });
        a.client.Craft(Crafting::Find("tank"), 1);
        Check(both(10.0, [&] { return (a.client.GetArmour() & Armour::Bit(ArmourPiece::OxygenTank)) != 0; }), "the diver makes an oxygen tank, and wears it");
        server.QueueConsoleCommand("tp Diver 20.5 -2 20.5"); // under water again
        Check(both(10.0, [&] { return a.client.OnTank(); }), "under water they breathe from the tank");
        both(4.0, [] { return false; });
        Check(a.client.OnTank() && a.client.GetAir() == a.client.GetMaxAir() && a.client.GetTank() < 0.98f && a.client.GetHealth() == 20,
              "their breath stays full while the tank empties (" + std::to_string(static_cast<int>(a.client.GetTank() * 100)) + "%)");
        server.QueueConsoleCommand("tp Diver 8.5 0 8.5");
        const bool refilled = both(20.0, [&] { return !a.client.OnTank() && a.client.GetTank() > 0.999f; });
        Check(refilled,
              "back in good air, the tank fills up again (" + std::to_string(static_cast<int>(a.client.GetTank() * 100)) + "%" +
                  (a.client.OnTank() ? ", still on it" : "") + ")");

    }
    if (sections.Run("air")) {
        std::cout << "Creative" << std::endl;
        server.QueueConsoleCommand("gamemode creative Diver");
        Check(both(20.0, [&] { return a.client.GetAirCause() == Cause::None && a.client.IsCreative(); }), "in creative, nothing takes your breath");
        Check(both(20.0, [&] { return a.client.GetAir() == a.client.GetMaxAir(); }), "and it comes back even down there");

    }
    a.client.Disconnect();
    builder.client.Disconnect();
    Pump({ &a, &builder }, 0.3, [] { return false; });
    server.Stop();
    if (!sections.Valid()) failures++;
    std::cout << (failures ? "BREATH TEST FAILED (" : "ALL BREATH CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
