// Traps, health and abilities over real connections: a server and two clients in an
// empty world, with physics running on the clients. Checks that
//  - spikes hurt, death is announced and respawns the player at the spawn point
//    with full health,
//  - crumbling bricks give way under a player, and the collapse spreads,
//  - a pressure plate opens a sluice gate on its channel, and not one without one,
//  - no-clip is an operator's command, the server tells the client, and it keeps
//    traps from hurting,
//  - a player who isn't an operator can't use the laser; an operator can.
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "TestSections.h"
#include "app/Player.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Signals.h"
#include "world/Collision.h"

// Test clients ask for a short view (chunks): plenty for the checks, and light enough
// to run every suite at once.
constexpr int kTestViewDistance = 4;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

struct TestClient {
    ClientWorld world;
    Client client{ world };
    Player player{ glm::vec3(0.0f) };
    PlayerInput input; // (walking, jumping: none unless a check sets it)
    bool joined = false;

    void Update(float dt) {
        client.Poll();
        if (!joined && client.IsWelcomed()) {
            joined = true;
            player.Teleport(client.GetSpawn());
            player.ResetSpawn();
        }
        if (auto c = client.TakeCorrection()) {
            // As the game does: move there and fall, unless that's inside blocks.
            player.Teleport(*c);
            if (!player.IsFlying() && Collision::BoxBlocked(world, player.GetBounds())) player.ResetSpawn();
        }
        if (!joined) return;
        const bool noclip = client.GetAbilities() & net::kAbilityNoclip;
        if (noclip != player.IsFlying()) player.SetFlying(world, noclip);
        if (!player.IsSpawned() && !player.IsFlying()) player.Spawn(world);
        player.Update(world, input, dt);
        world.Update(player.GetEyePosition());
        if (player.IsSpawned() || player.IsFlying())
            client.SendPlayerState(player.GetFeetPosition(), player.GetVelocity(), 0.0f, 0.0f, player.IsFlying() ? net::kFlagFlying : 0);
    }
};

bool Pump(std::initializer_list<TestClient*> clients, double seconds, const std::function<bool()>& done) {
    auto last = std::chrono::steady_clock::now();
    const auto deadline = last + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        for (auto* c : clients) c->Update(dt);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

BlockType TypeAt(TestClient& c, const glm::ivec3& p) {
    Block b;
    return c.world.GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT;
}

bool Has(const TestClient& c, const std::string& text) {
    for (const auto& line : c.client.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

int main(int argc, char** argv) {
    TestSections sections(argc, argv);
    Server server;
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = false;
    options.worldType = TerrainGenerator::Type::Empty; // flat ground: the top of row -1 is at 0
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival; // traps only hurt in survival
    if (!server.Start(options)) return EXIT_FAILURE;
    server.RunInBackground();

    TestClient a, b;
    std::string error;
    a.client.Connect("127.0.0.1", server.GetPort(), "Tester", error, kTestViewDistance);
    b.client.Connect("127.0.0.1", server.GetPort(), "Watcher", error, kTestViewDistance);
    auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &b }, s, done); };
    Check(both(30.0, [&] { return a.player.IsSpawned() && b.player.IsSpawned(); }), "both players land on the ground");
    Check(a.client.GetHealth() == 20 && a.client.GetMaxHealth() == 20, "and start with full health (10 hearts)");
    // The tester builds the traps in survival, so needs the blocks; water can't be
    // placed in survival, so the watcher (in creative) pours it.
    for (const char* give : { "give spikes 4 Tester", "give crumbling_brick 4 Tester", "give sluice 2 Tester", "give pressure_plate 1 Tester" })
        server.QueueConsoleCommand(give);
    server.QueueConsoleCommand("gamemode creative Watcher");
    Check(both(20.0, [&] { return a.client.GetResources(BlockType::SPIKES) == 4 * 64 && a.client.GetResources(BlockType::PRESSURE_PLATE) == 64 && b.client.IsCreative(); }),
          "the tester is given trap blocks, the watcher goes creative");

    if (sections.Run("traps")) {
        std::cout << "Spikes" << std::endl;
        const glm::ivec3 feet(glm::floor(a.player.GetFeetPosition()));
        a.client.PlaceBlock(feet, BlockType::SPIKES);
        Check(both(20.0, [&] { return a.client.GetHealth() < 20; }) && a.client.GetLastHurt() > 0.0,
              "standing in spikes hurts (" + std::to_string(a.client.GetHealth()) + " left)");
        Check(both(20.0, [&] { return Has(a, "Tester was impaled on spikes") && Has(b, "Tester was impaled on spikes"); }), "until death, which everyone hears about");
        // Mined (it takes time in survival) before falling back onto them.
        a.client.StartMining(feet, Tool::Pickaxe);
        both(Mining::Seconds(Block::Plant(BlockType::SPIKES, 0), Tool::Pickaxe) + 0.1, [] { return false; });
        for (int attempt = 0; attempt < 10 && TypeAt(b, feet) == BlockType::SPIKES; attempt++) {
            a.client.BreakBlock(feet, Tool::Pickaxe); // again if the server was too busy to see the mining in time
            both(1.0, [&] { return TypeAt(b, feet) != BlockType::SPIKES; });
        }
        Check(both(20.0, [&] { return a.client.GetHealth() == 20 && glm::distance(glm::vec2(a.player.GetFeetPosition().x, a.player.GetFeetPosition().z), glm::vec2(7.5f)) < 1.0f; }),
              "then respawns at the spawn point with full health");
        // Respawning drops the player from high up: wait for the landing (building has a reach).
        Check(both(15.0, [&] { return a.player.GetFeetPosition().y < 0.5f && a.player.IsOnGround(); }), "and lands");
        Check(Has(a, "in a chest where you died") && a.client.GetResources(BlockType::CRUMBLING_BRICK) == 2 * 64,
              "having left half of what they carried in a chest where they died");
        // (Half of the trap blocks are in that chest now: more, for the traps below.)
        for (const char* give : { "give spikes 4 Tester", "give crumbling_brick 2 Tester", "give sluice 1 Tester", "give pressure_plate 1 Tester" })
            server.QueueConsoleCommand(give);
        both(20.0, [&] { return a.client.GetResources(BlockType::CRUMBLING_BRICK) == 4 * 64 && a.client.GetResources(BlockType::PRESSURE_PLATE) >= 64; });

        std::cout << "Crumbling bricks" << std::endl;
        // (Survival players build within 6 blocks: go there first.)
        server.QueueConsoleCommand("tp Tester 21.5 0 5.5");
        both(20.0, [&] { return std::abs(a.player.GetFeetPosition().x - 21.5f) < 0.2f && a.player.IsOnGround(); });
        for (int x = 20; x <= 23; x++) a.client.PlaceBlock({ x, 4, 7 }, BlockType::CRUMBLING_BRICK);
        both(1.0, [] { return false; });
        Check(TypeAt(b, { 20, 4, 7 }) == BlockType::CRUMBLING_BRICK && TypeAt(b, { 23, 4, 7 }) == BlockType::CRUMBLING_BRICK, "a ledge of crumbling bricks is built");
        server.QueueConsoleCommand("tp Tester 20.5 5.2 7.5");
        Check(both(20.0, [&] { return TypeAt(b, { 20, 4, 7 }) == BlockType::AIR; }), "it gives way under the player");
        Check(both(20.0, [&] { return TypeAt(b, { 23, 4, 7 }) == BlockType::AIR; }), "and the collapse spreads along the ledge");
        Check(both(20.0, [&] { return a.player.GetFeetPosition().y < 1.0f; }), "dropping the player to the ground");

        std::cout << "Pressure plates" << std::endl;
        server.QueueConsoleCommand("tp Tester 32.5 0 8.5");
        both(20.0, [&] { return std::abs(a.player.GetFeetPosition().x - 32.5f) < 0.2f && a.player.IsOnGround(); });
        a.client.PlaceBlock({ 30, 3, 7 }, BlockType::SLUICE);
        b.client.PlaceBlock({ 30, 4, 7 }, BlockType::WATER);
        a.client.PlaceBlock({ 30, 0, 10 }, BlockType::PRESSURE_PLATE);
        b.client.PlaceBlock({ 31, 3, 7 }, BlockType::SLUICE); // with no channel (the creative watcher reaches that far)
        both(1.0, [] { return false; });
        Check(TypeAt(b, { 30, 3, 7 }) == BlockType::SLUICE && TypeAt(b, { 30, 0, 10 }) == BlockType::PRESSURE_PLATE, "a sluice gate holds water up; a plate is set nearby");
        // The plate sends on channel 4, and the first gate listens to it (world/Signals.h).
        net::SignalInfo plate, gate;
        plate.pos = { 30, 0, 10 };
        plate.kind = static_cast<uint8_t>(Signals::Kind::Plate);
        plate.out = 4;
        gate.pos = { 30, 3, 7 };
        gate.kind = static_cast<uint8_t>(Signals::Kind::Sluice);
        gate.a = 4;
        b.client.ConfigureSignal(plate);
        b.client.ConfigureSignal(gate);
        Check(both(20.0, [&] { return b.client.GetSignals().count(plate.pos) && b.client.GetSignals().count(gate.pos); }), "the plate sends on 4, and the gate listens to 4");
        server.QueueConsoleCommand("tp Tester 30.5 1 10.5");
        Check(both(20.0, [&] { return TypeAt(b, { 30, 3, 7 }) == BlockType::AIR && Has(a, "*click*"); }), "stepping on the plate opens the gate");
        Check(TypeAt(b, { 31, 3, 7 }) == BlockType::SLUICE, "but not one with no channel (they don't open the old way)");
        Check(both(20.0, [&] { Block w; return b.world.GetBlock(30, 4, 7, w) && !w.HasWater(); }), "and the water it held pours out");

    }
    if (sections.Run("falling")) {
        std::cout << "Falling" << std::endl;
        server.QueueConsoleCommand("tp Tester 60.5 0 7.5");
        both(20.0, [&] { return std::abs(a.player.GetFeetPosition().x - 60.5f) < 0.2f && a.player.IsOnGround(); });
        both(6.0, [&] { return a.client.GetHealth() == 20; }); // (healed up from the traps)
        const int beforeFall = a.client.GetHealth();
        server.QueueConsoleCommand("tp Tester 60.5 3 7.5"); // a 3-block drop is safe
        both(20.0, [&] { return a.player.GetFeetPosition().y < 0.5f && a.player.IsOnGround(); });
        both(1.0, [] { return false; });
        Check(a.client.GetHealth() == beforeFall, "a 3-block drop doesn't hurt");
        server.QueueConsoleCommand("tp Tester 60.5 11 7.5");
        both(20.0, [&] { return a.player.GetFeetPosition().y < 0.5f && a.player.IsOnGround(); });
        both(1.0, [] { return false; });
        Check(std::abs(a.client.GetHealth() - (beforeFall - 8)) <= 1, "an 11-block fall costs 4 hearts (health " + std::to_string(a.client.GetHealth()) + ")");
        server.QueueConsoleCommand("food 20 Tester"); // (healing needs food)
        both(40.0, [&] { return a.client.GetHealth() == 20; }); // (healed again before the checks below)
        // Down a mountainside: twelve one-block steps, run down sprinting: no single drop is
        // more than a step, so it doesn't hurt (it used to count as one twelve-block fall).
        for (int i = 0; i < 12; i++)
            server.QueueConsoleCommand("fill " + std::to_string(80 + i) + " 0 5 " + std::to_string(80 + i) + " " + std::to_string(11 - i) + " 10 stone");
        server.QueueConsoleCommand("tp Tester 80.5 13 7.5");
        both(20.0, [&] { return std::abs(a.player.GetFeetPosition().x - 80.5f) < 0.3f && a.player.IsOnGround() && a.player.GetFeetPosition().y > 11.5f; });
        const int beforeRun = a.client.GetHealth();
        a.input.move = glm::vec2(0.0f, 1.0f);
        a.input.yaw = 0.0f; // (along +x)
        a.input.lookDirection = glm::vec3(1.0f, 0.0f, 0.0f);
        a.input.sprint = true;
        both(20.0, [&] { return a.player.GetFeetPosition().x > 93.0f; });
        a.input = PlayerInput{};
        both(1.5, [] { return false; });
        Check(a.player.GetFeetPosition().y < 1.5f && a.client.GetHealth() == beforeRun,
              "sprinting down twelve one-block steps doesn't hurt (health " + std::to_string(a.client.GetHealth()) + ", at y " +
                  std::to_string(a.player.GetFeetPosition().y) + ")");

    }
    if (sections.Run("tnt")) {
        std::cout << "TNT" << std::endl;
        server.QueueConsoleCommand("tp Tester 66.5 0 7.5");
        server.QueueConsoleCommand("give tnt 1 Tester");
        both(20.0, [&] { return std::abs(a.player.GetFeetPosition().x - 66.5f) < 0.2f && a.player.IsOnGround() && a.client.GetResources(BlockType::TNT) == 64; });
        a.client.PlaceBlock({ 70, 0, 7 }, BlockType::TNT);
        both(20.0, [&] { return TypeAt(b, { 70, 0, 7 }) == BlockType::TNT; });
        const int beforeBlast = a.client.GetHealth();
        a.client.UseBed({ 70, 0, 7 });
        Check(both(20.0, [&] { return TypeAt(b, { 70, 0, 7 }) == BlockType::TNT_LIT; }), "right-clicking TNT lights it");
        Check(both(20.0, [&] { return TypeAt(b, { 70, -1, 7 }) == BlockType::AIR && TypeAt(b, { 70, -3, 7 }) == BlockType::AIR; }), "a few seconds later it blows a crater");
        Check(TypeAt(b, { 70, -5, 7 }) != BlockType::AIR, "about three blocks across");
        Check(both(5.0, [&] { return a.client.GetHealth() < beforeBlast; }), "and hurts whoever's close");
        server.QueueConsoleCommand("food 20 Tester");
        server.QueueConsoleCommand("tp Tester 60.5 0 7.5");
        both(40.0, [&] { return a.client.GetHealth() == 20; });

        std::cout << "No-clip and the laser" << std::endl;
        a.client.SendChat("/noclip");
        Check(both(20.0, [&] { return Has(a, "Only operators can do that"); }) && !(a.client.GetAbilities() & net::kAbilityNoclip), "a player can't turn on no-clip");
        const glm::ivec3 target(40, -1, 7);
        a.client.Carve({ target }, 1);
        both(1.0, [] { return false; });
        Check(TypeAt(b, target) != BlockType::AIR, "or use the laser: the server ignores it");
        server.QueueConsoleCommand("op Tester");
        Check(both(20.0, [&] { return a.client.GetAbilities() & net::kAbilityOperator; }), "once made an operator, the client is told");
        a.client.Carve({ target }, 1);
        Check(both(20.0, [&] { return TypeAt(b, target) == BlockType::AIR; }), "and the laser works");
        a.client.SendChat("/noclip");
        Check(both(20.0, [&] { return (a.client.GetAbilities() & net::kAbilityNoclip) && a.player.IsFlying(); }), "/noclip turns it on, and the client flies");
        both(0.5, [] { return false; }); // (so the server has heard they're flying)
        const glm::ivec3 here(glm::floor(a.player.GetFeetPosition()));
        a.client.PlaceBlock(here, BlockType::SPIKES);
        both(2.0, [] { return false; });
        Check(a.client.GetHealth() == 20, "spikes can't hurt a player in no-clip (health " + std::to_string(a.client.GetHealth()) + ")");
        server.QueueConsoleCommand("deop Tester");
        Check(both(20.0, [&] { return !(a.client.GetAbilities() & (net::kAbilityNoclip | net::kAbilityOperator)) && !a.player.IsFlying(); }),
              "losing operator status ends no-clip too");

    }
    a.client.Disconnect();
    b.client.Disconnect();
    server.Stop();
    if (!sections.Valid()) failures++;
    std::cout << (failures ? "TRAP TEST FAILED (" : "ALL TRAP CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
