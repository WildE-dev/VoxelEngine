// Creatures over real connections. Checks that
//  - /summon brings deer and ghouls, the players near them see them, and they fall
//    onto the ground,
//  - a ghoul comes for a survival player and hurts them, and leaves creative players
//    alone,
//  - hitting a deer knocks it back and sends it running, enough hits kill it, and
//    hits out of reach or too fast don't count,
//  - ghouls burn up in daylight under the open sky,
//  - /creatures counts and clears them,
//  - in a normal world, deer turn up by themselves on grass, and ghouls come out at
//    night,
//  - PvP: players hit each other in reach (flashing red for others, knocked back), can be slain,
//    and /pvp off stops it,
//  - archery: an arrow hits a player ten blocks off, a bow's shots are spaced out,
//    and arrows stick in the ground.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"

// Test clients ask for a short view (chunks): plenty for the checks, and light enough
// to run every suite at once.
constexpr int kTestViewDistance = 4;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

// A client that stays wherever the server puts it.
struct TestClient {
    ClientWorld world;
    Client client{ world };
    glm::vec3 feet{ 0.0f };
    bool joined = false;
    Tool heldTool = Tool::None; // what others see in their hand
    BlockType heldBlock = BlockType::AIR;
    void Update() {
        client.Poll();
        if (!joined && client.IsWelcomed()) joined = true, feet = client.GetSpawn();
        if (auto c = client.TakeCorrection()) feet = *c;
        if (!joined) return;
        world.Update(feet + glm::vec3(0.0f, 1.62f, 0.0f));
        client.SendPlayerState(feet, glm::vec3(0.0f), 0.0f, 0.0f, net::kFlagOnGround, heldTool, heldBlock);
    }
    const Client::Creature* Find(CreatureType type) const {
        static thread_local std::vector<Client::Creature> seen;
        seen = client.GetCreatures();
        for (const auto& c : seen)
            if (c.type == type) return &c;
        return nullptr;
    }
};

bool Pump(std::initializer_list<TestClient*> clients, double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto* c : clients) c->Update();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

bool Has(const TestClient& c, const std::string& text) {
    for (const auto& line : c.client.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

Server::Options BaseOptions(TerrainGenerator::Type type) {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = false;
    options.worldType = type;
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Survival;
    options.dayLengthSeconds = 100000.0f; // the clock stays where /time puts it
    return options;
}

} // namespace

int main() {
    {
        Server server;
        if (!server.Start(BaseOptions(TerrainGenerator::Type::Empty))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a, b;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Hunter", error, kTestViewDistance);
        b.client.Connect("127.0.0.1", server.GetPort(), "Watcher", error, kTestViewDistance);
        auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &b }, s, done); };
        auto wait = [&](double s) { both(s, [] { return false; }); };
        Check(both(20.0, [&] { return a.joined && b.joined; }), "two players join an empty world");
        server.QueueConsoleCommand("time midnight"); // no burning for now
        server.QueueConsoleCommand("gamemode creative Watcher");
        server.QueueConsoleCommand("tp Hunter 8.5 0 8.5");
        server.QueueConsoleCommand("tp Watcher 30.5 0 8.5");
        both(20.0, [&] { return b.client.IsCreative(); });
        wait(0.5);
        Check(a.client.GetCreatures().empty(), "an empty world has no creatures of its own");

        std::cout << "Summoning" << std::endl;
        server.QueueConsoleCommand("summon deer 12.5 3 8.5");
        Check(both(20.0, [&] { return a.Find(CreatureType::Deer) && b.Find(CreatureType::Deer); }), "/summon brings a deer, and both players see it");
        Check(both(20.0, [&] { const auto* d = a.Find(CreatureType::Deer); return d && std::abs(d->feet.y) < 0.05f; }), "it falls onto the ground");

        std::cout << "Fighting" << std::endl;
        const uint16_t deer = a.Find(CreatureType::Deer)->id;
        server.QueueConsoleCommand("tp Hunter 30.5 0 30.5"); // far away
        wait(1.0);
        a.client.Attack(deer, Tool::Axe);
        wait(1.0);
        Check(a.client.GetChat().empty() || !Has(a, "defeated"), "a hit from 20 blocks away doesn't count");
        const auto* d = a.Find(CreatureType::Deer);
        server.QueueConsoleCommand("tp Hunter " + std::to_string(d->feet.x - 2.0f) + " 0 " + std::to_string(d->feet.z));
        both(20.0, [&] { return glm::distance(a.feet, glm::vec3(d->feet.x - 2.0f, 0.0f, d->feet.z)) < 0.5f; });
        wait(0.5);
        const glm::vec3 hitAt = a.Find(CreatureType::Deer)->feet;
        a.client.Attack(deer, Tool::Axe);
        Check(both(20.0, [&] { const auto* c = a.Find(CreatureType::Deer); return c && c->hurt; }), "a hit in reach lands: the deer flashes red");
        Check(both(20.0, [&] { const auto* c = a.Find(CreatureType::Deer); return c && c->feet.x > hitAt.x + 3.0f; }), "and it runs off, away from the hunter");
        a.client.Attack(deer, Tool::Axe); // too soon after the first
        wait(0.6);
        Check(a.Find(CreatureType::Deer) != nullptr, "two hits at once count as one");
        const auto* runner = a.Find(CreatureType::Deer);
        server.QueueConsoleCommand("tp Hunter " + std::to_string(runner->feet.x - 1.5f) + " " + std::to_string(runner->feet.y) + " " +
                                   std::to_string(runner->feet.z));
        bool gone = false;
        for (int i = 0; i < 10 && !gone; i++) {
            wait(0.5);
            const auto* c = a.Find(CreatureType::Deer);
            if (!c) { gone = true; break; }
            server.QueueConsoleCommand("tp Hunter " + std::to_string(c->feet.x - 1.5f) + " " + std::to_string(c->feet.y) + " " + std::to_string(c->feet.z));
            wait(0.4);
            a.client.Attack(deer, Tool::Axe);
            gone = both(1.0, [&] { return a.Find(CreatureType::Deer) == nullptr; });
        }
        Check(gone && both(5.0, [&] { return Has(a, "You defeated a deer"); }), "a second axe hit (8 health, 5 a hit) finishes it");

        std::cout << "Ghouls" << std::endl;
        server.QueueConsoleCommand("tp Hunter 8.5 0 8.5");
        server.QueueConsoleCommand("tp Watcher 8.5 0 40.5");
        wait(1.0);
        server.QueueConsoleCommand("summon ghoul 16.5 0 8.5");
        Check(both(20.0, [&] { return a.Find(CreatureType::Ghoul) != nullptr; }), "a ghoul is summoned 8 blocks from the hunter");
        Check(both(20.0, [&] { return a.client.GetHealth() < 20; }), "it comes for the hunter (in survival) and hurts them");
        server.QueueConsoleCommand("gamemode creative Hunter");
        both(20.0, [&] { return a.client.IsCreative(); });
        const int health = a.client.GetHealth();
        wait(3.0);
        Check(a.client.GetHealth() >= health, "a creative player is left alone");
        server.QueueConsoleCommand("time noon");
        Check(both(20.0, [&] { return a.Find(CreatureType::Ghoul) == nullptr; }), "at noon under the open sky the ghoul burns up");

        std::cout << "Skeletons" << std::endl;
        server.QueueConsoleCommand("time midnight");
        server.QueueConsoleCommand("gamemode survival Hunter");
        server.QueueConsoleCommand("food 20 Hunter");
        server.QueueConsoleCommand("tp Hunter 8.5 0 8.5");
        server.QueueConsoleCommand("summon skeleton 20.5 0 8.5");
        Check(both(20.0, [&] { return a.Find(CreatureType::Skeleton) != nullptr && !a.client.IsCreative(); }), "a skeleton is summoned 12 blocks from the hunter");
        both(4.0, [&] { return a.client.GetHealth() == 20; });
        const int beforeVolley = a.client.GetHealth();
        Check(both(20.0, [&] { return a.client.GetHealth() < beforeVolley; }), "it shoots the hunter from a distance");
        const auto* skeleton = a.Find(CreatureType::Skeleton);
        Check(skeleton && glm::distance(glm::vec2(skeleton->feet.x, skeleton->feet.z), glm::vec2(8.5f)) > 5.0f, "and keeps its distance");
        server.QueueConsoleCommand("creatures clear");
        server.QueueConsoleCommand("gamemode creative Hunter");
        both(20.0, [&] { return a.client.GetCreatures().empty() && a.client.IsCreative(); });

        std::cout << "Wolves" << std::endl;
        server.QueueConsoleCommand("time noon");
        server.QueueConsoleCommand("tp Hunter 8.5 0 8.5");
        server.QueueConsoleCommand("summon wolf 11.5 0 8.5");
        server.QueueConsoleCommand("give raw_venison 2 Hunter");
        Check(both(20.0, [&] { return a.Find(CreatureType::Wolf) != nullptr; }), "a wolf is summoned beside the hunter");
        const uint16_t wolf = a.Find(CreatureType::Wolf)->id;
        a.client.Interact(wolf); // (empty-handed: nothing)
        both(1.0, [] { return false; });
        Check(!a.Find(CreatureType::Wolf)->tame, "a wolf isn't tamed by an empty hand");
        a.heldBlock = BlockType::RAW_VENISON;
        both(0.3, [] { return false; });
        a.client.Interact(wolf);
        Check(both(20.0, [&] { const auto* w = a.Find(CreatureType::Wolf); return w && w->tame; }) && Has(a, "The wolf is yours"), "fed raw venison, it's tamed (and wears a collar)");
        a.heldBlock = BlockType::AIR;
        server.QueueConsoleCommand("tp Hunter 22.5 0 8.5");
        Check(both(20.0, [&] { const auto* w = a.Find(CreatureType::Wolf); return w && glm::distance(glm::vec2(w->feet.x, w->feet.z), glm::vec2(22.5f, 8.5f)) < 5.0f; }),
              "it follows its owner");
        server.QueueConsoleCommand("time midnight");
        server.QueueConsoleCommand("summon ghoul 26.5 0 8.5");
        Check(both(20.0, [&] { return a.Find(CreatureType::Ghoul) != nullptr; }), "a ghoul turns up near them");
        Check(both(30.0, [&] { return a.Find(CreatureType::Ghoul) == nullptr; }), "and the wolf sees it off");
        server.QueueConsoleCommand("creatures clear");
        both(20.0, [&] { return a.client.GetCreatures().empty(); });

        server.QueueConsoleCommand("time midnight");
        server.QueueConsoleCommand("summon ghoul 20 0 20");
        server.QueueConsoleCommand("summon deer 22 0 20");
        server.QueueConsoleCommand("creatures");
        wait(1.0);
        server.QueueConsoleCommand("creatures clear");
        Check(both(20.0, [&] { return a.client.GetCreatures().empty(); }), "/creatures clear removes them all");
        a.client.Disconnect();
        b.client.Disconnect();
        Pump({ &a, &b }, 0.3, [] { return false; });
        server.Stop();
    }

    std::cout << "PvP" << std::endl;
    {
        Server server;
        if (!server.Start(BaseOptions(TerrainGenerator::Type::Empty))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a, b;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Duelist", error, kTestViewDistance);
        b.client.Connect("127.0.0.1", server.GetPort(), "Rival", error, kTestViewDistance);
        auto both = [&](double s, const std::function<bool()>& done) { return Pump({ &a, &b }, s, done); };
        auto wait = [&](double s) { both(s, [] { return false; }); };
        Check(both(20.0, [&] { return a.joined && b.joined; }), "two survival players join");
        const uint16_t rival = b.client.GetPlayerId();
        auto rivalAsSeen = [&]() -> Client::RemotePlayer {
            for (const auto& p : a.client.GetRemotePlayers())
                if (p.id == rival) return p;
            return {};
        };
        auto duelistAsSeen = [&]() -> Client::RemotePlayer {
            for (const auto& p : b.client.GetRemotePlayers())
                if (p.id == a.client.GetPlayerId()) return p;
            return {};
        };
        Check(both(20.0, [&] { return rivalAsSeen().name == "Rival" && duelistAsSeen().name == "Duelist"; }),
              "each sees the other's name (for name tags), whoever joined first");
        auto rivalSeenHurt = [&] {
            for (const auto& p : a.client.GetRemotePlayers())
                if (p.id == rival && (p.flags & net::kFlagHurt)) return true;
            return false;
        };
        server.QueueConsoleCommand("tp Duelist 8.5 0 8.5");
        server.QueueConsoleCommand("tp Rival 30.5 0 8.5");
        both(20.0, [&] { return std::abs(b.feet.x - 30.5f) < 0.1f && std::abs(a.feet.x - 8.5f) < 0.1f; });
        wait(0.5);
        a.client.AttackPlayer(rival, Tool::None);
        wait(1.0);
        Check(b.client.GetHealth() == 20, "a hit from 22 blocks away doesn't count");
        server.QueueConsoleCommand("tp Rival 10.5 0 8.5");
        both(20.0, [&] { return std::abs(b.feet.x - 10.5f) < 0.1f; });
        wait(0.5);
        a.client.AttackPlayer(rival, Tool::None);
        Check(both(20.0, [&] { return b.client.GetHealth() == 18; }), "a bare-handed hit in reach takes a heart");
        Check(both(5.0, rivalSeenHurt), "and the hitter sees them flash red");
        glm::vec3 push(0.0f);
        both(5.0, [&] { push += b.client.TakeKnockback(); return push.x > 0.0f; });
        Check(push.x > 3.0f && push.y > 0.0f && std::abs(push.z) < 0.1f, "and they're knocked back, away from the hitter and up");
        server.QueueConsoleCommand("pvp off");
        wait(1.0);
        a.client.AttackPlayer(rival, Tool::None);
        wait(1.0);
        Check(b.client.GetHealth() == 18 && Has(a, "PvP is off"), "with /pvp off, hits do nothing (and the hitter is told)");
        server.QueueConsoleCommand("pvp on");
        server.QueueConsoleCommand("give stone 4 Rival"); // (half is left behind when they die)
        both(20.0, [&] { return b.client.GetResources(BlockType::STONE) == 4 * 64; });
        for (int i = 0; i < 12 && b.client.GetHealth() > 0 && !Has(b, "slain"); i++) {
            server.QueueConsoleCommand("tp Rival " + std::to_string(a.feet.x + 2.0f) + " 0 8.5");
            wait(0.5);
            a.client.AttackPlayer(rival, Tool::Axe);
            wait(0.5);
        }
        Check(both(5.0, [&] { return Has(b, "Rival was slain by Duelist"); }), "enough axe hits slay them (and everyone's told)");
        Check(both(5.0, [&] { return b.client.GetHealth() == 20; }), "and they're back at full health");
        Check(both(5.0, [&] { return b.client.GetResources(BlockType::STONE) == 2 * 64 && Has(b, "in a chest where you died"); }),
              "keeping half their stone; the other half is left where they died");
        glm::ivec3 grave(0);
        for (const auto& line : b.client.GetChat())
            if (const size_t at = line.text.find("died ("); at != std::string::npos)
                std::sscanf(line.text.c_str() + at + 6, "%d, %d, %d", &grave.x, &grave.y, &grave.z);
        Block left;
        Check(both(20.0, [&] { return a.world.GetBlock(grave.x, grave.y, grave.z, left) && left.type == BlockType::CHEST; }),
              "in a chest the killer can see");
        server.QueueConsoleCommand("tp Duelist " + std::to_string(grave.x - 1.5f) + " 0 " + std::to_string(grave.z + 0.5f));
        both(20.0, [&] { return std::abs(a.feet.x - (grave.x - 1.5f)) < 0.1f; });
        a.client.OpenChest(grave);
        Check(both(20.0, [&] { return a.client.GetChestUnits(BlockType::STONE) == 2 * 64; }), "and open, with the 2 stone in it");
        a.client.CloseChest();
        server.QueueConsoleCommand("give sword Duelist");
        for (const char* piece : { "cap", "tunic", "boots" }) server.QueueConsoleCommand(std::string("give ") + piece + " Rival");
        server.QueueConsoleCommand("tp Rival " + std::to_string(a.feet.x + 2.0f) + " 0 8.5");
        both(20.0, [&] { return b.client.HasArmour(ArmourPiece::Boots) && a.client.HasTool(Tool::Sword); });
        a.heldTool = Tool::Sword;
        Check(both(20.0, [&] { return duelistAsSeen().heldTool == Tool::Sword && rivalAsSeen().armour == 7; }),
              "others see the sword in the duelist's hand and the rival's armour");
        wait(3.5); // (past their respawn grace)
        a.client.AttackPlayer(rival, Tool::Sword);
        Check(both(20.0, [&] { return b.client.GetHealth() < 20; }) && b.client.GetHealth() == 16,
              "armour takes a sword's 3.5 hearts down to 2 (health " + std::to_string(b.client.GetHealth()) + ")");

        std::cout << "Archery" << std::endl;
        server.QueueConsoleCommand("give bow Duelist");
        server.QueueConsoleCommand("give arrow 3 Duelist");
        server.QueueConsoleCommand("tp Duelist 20.5 0 8.5");
        server.QueueConsoleCommand("tp Rival 30.5 0 8.5"); // ten blocks off
        Check(both(20.0, [&] { return a.client.HasTool(Tool::Bow) && a.client.GetResources(BlockType::ARROW) == 3 * 64 && std::abs(b.feet.x - 30.5f) < 0.1f; }),
              "the duelist has a bow and three arrows; the rival stands ten blocks off");
        both(3.5, [] { return false; }); // (no grace left; healed or not)
        const int beforeArrow = b.client.GetHealth();
        // Aim at their middle, a little high (arrows drop).
        const glm::vec3 eye = a.feet + glm::vec3(0.0f, 1.62f, 0.0f), target = b.feet + glm::vec3(0.0f, 1.0f + 0.5f, 0.0f);
        a.client.Shoot(target - eye);
        Check(both(5.0, [&] { return b.client.GetHealth() < beforeArrow; }) && b.client.GetHealth() == beforeArrow - 3,
              "an arrow across ten blocks hits (2.5 hearts, armour takes it to 1.5)");
        Check(a.client.GetResources(BlockType::ARROW) == 2 * 64, "and uses an arrow");
        a.client.Shoot(glm::vec3(1.0f, -1.0f, 0.0f)); // (too soon: the bow isn't ready)
        both(1.0, [] { return false; });
        Check(a.client.GetResources(BlockType::ARROW) == 2 * 64, "a second shot straight after doesn't go");
        a.client.Shoot(glm::vec3(1.0f, -1.0f, 0.0f)); // at the ground ahead
        bool stuck = false;
        both(5.0, [&] {
            for (const auto& arrow : a.client.GetArrows()) stuck |= glm::length(arrow.velocity) < 1e-3f;
            return stuck;
        });
        Check(stuck && a.client.GetResources(BlockType::ARROW) == 64, "one shot at the ground sticks there");
        a.client.Disconnect();
        b.client.Disconnect();
        Pump({ &a, &b }, 0.3, [] { return false; });
        server.Stop();
    }

    std::cout << "Natural spawning" << std::endl;
    {
        Server server;
        if (!server.Start(BaseOptions(TerrainGenerator::Type::Normal))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Wanderer", error, kTestViewDistance);
        Check(Pump({ &a }, 30.0, [&] { return a.joined; }), "a player joins a normal world (forest at spawn)");
        server.QueueConsoleCommand("gamemode creative Wanderer");
        server.QueueConsoleCommand("time noon");
        Check(Pump({ &a }, 60.0, [&] { return a.Find(CreatureType::Deer) != nullptr; }), "deer turn up by themselves around the player");
        server.QueueConsoleCommand("time midnight");
        Check(Pump({ &a }, 60.0, [&] { return a.Find(CreatureType::Ghoul) != nullptr; }), "ghouls come out at night");
        a.client.Disconnect();
        Pump({ &a }, 0.3, [] { return false; });
        server.Stop();
    }

    std::cout << (failures ? "CREATURE TEST FAILED (" : "ALL CREATURE CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
