// Creatures over real connections. Checks that
//  - /summon brings deer and dwarves, the players near them see them, and they fall
//    onto the ground,
//  - a dwarf warrior comes for a survival player and hurts them, and leaves creative
//    players alone; daylight doesn't hurt it,
//  - a dwarf crossbowman shoots from a distance,
//  - hitting a deer knocks it back and sends it running, enough hits kill it, and
//    hits out of reach or too fast don't count,
//  - /creatures counts and clears them,
//  - in a normal world, deer turn up by themselves on grass, and dwarves come out at
//    a dwarf camp or mine when a player comes near it,
//  - PvP: players hit each other in reach (flashing red for others, knocked back), can be slain,
//    and /pvp off stops it,
//  - archery: an arrow hits a player ten blocks off, a bow's shots are spaced out,
//    and arrows stick in the ground.
#include <algorithm>
#include <chrono>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "TestSections.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Trades.h"
#include "world/Dwarves.h"
#include "world/TerrainGenerator.h"

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
    ItemRef heldBlock;
    bool crouching = false;
    void Update() {
        client.Poll();
        if (!joined && client.IsWelcomed()) joined = true, feet = client.GetSpawn();
        if (auto c = client.TakeCorrection()) feet = *c;
        if (!joined) return;
        world.Update(feet + glm::vec3(0.0f, 1.62f, 0.0f));
        client.SendPlayerState(feet, glm::vec3(0.0f), 0.0f, 0.0f, net::kFlagOnGround | (crouching ? net::kFlagCrouching : 0), heldTool, heldBlock);
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

int main(int argc, char** argv) {
    TestSections sections(argc, argv);
    if (sections.Any("fighting", "allies", "raids")) {
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

        if (sections.Run("fighting")) {
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
            Check(a.Find(CreatureType::Deer) != nullptr && a.client.GetResources(ItemType::RAW_VENISON) == 0, "a hit from 20 blocks away doesn't count");
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
            Check(gone && both(5.0, [&] { return a.client.GetResources(ItemType::RAW_VENISON) > 0; }) && !Has(a, "defeated"),
                  "a second axe hit (8 health, 5 a hit) finishes it (its venison given, with nothing said in the chat)");

            std::cout << "Dwarf warriors" << std::endl;
            server.QueueConsoleCommand("tp Hunter 8.5 0 8.5");
            server.QueueConsoleCommand("tp Watcher 8.5 0 40.5");
            wait(1.0);
            server.QueueConsoleCommand("summon dwarf 16.5 0 8.5");
            Check(both(20.0, [&] { return a.Find(CreatureType::DwarfWarrior) != nullptr; }), "a dwarf warrior is summoned 8 blocks from the hunter");
            Check(both(20.0, [&] { return a.client.GetHealth() < 20; }), "it comes for the hunter (in survival) and hurts them");
            server.QueueConsoleCommand("gamemode creative Hunter");
            both(20.0, [&] { return a.client.IsCreative(); });
            const int health = a.client.GetHealth();
            wait(3.0);
            Check(a.client.GetHealth() >= health, "a creative player is left alone");
            server.QueueConsoleCommand("time noon");
            wait(4.0);
            Check(a.Find(CreatureType::DwarfWarrior) != nullptr, "daylight doesn't hurt it (dwarves don't burn)");
            server.QueueConsoleCommand("creatures clear");

            std::cout << "Dwarf crossbowmen" << std::endl;
            server.QueueConsoleCommand("time midnight");
            server.QueueConsoleCommand("gamemode survival Hunter");
            server.QueueConsoleCommand("food 20 Hunter");
            server.QueueConsoleCommand("tp Hunter 8.5 0 8.5");
            server.QueueConsoleCommand("summon crossbow_dwarf 20.5 0 8.5");
            Check(both(20.0, [&] { return a.Find(CreatureType::DwarfCrossbow) != nullptr && !a.client.IsCreative(); }), "a dwarf crossbowman is summoned 12 blocks from the hunter");
            both(4.0, [&] { return a.client.GetHealth() == 20; });
            const int beforeVolley = a.client.GetHealth();
            Check(both(20.0, [&] { return a.client.GetHealth() < beforeVolley; }), "it shoots the hunter from a distance");
            const auto* bowman = a.Find(CreatureType::DwarfCrossbow);
            Check(bowman && glm::distance(glm::vec2(bowman->feet.x, bowman->feet.z), glm::vec2(8.5f)) > 5.0f, "and keeps its distance");
            server.QueueConsoleCommand("creatures clear");
            server.QueueConsoleCommand("gamemode creative Hunter");
            both(20.0, [&] { return a.client.GetCreatures().empty() && a.client.IsCreative(); });

            std::cout << "Slopes" << std::endl;
            {
                // A pen of stone slabs three quarters high (natural ground: a slope, which
                // players can't walk into): a deer in it goes over them, not through them.
                server.QueueConsoleCommand("tp Hunter 11.5 0 12.5");
                both(10.0, [&] { return std::abs(a.feet.z - 12.5f) < 0.1f; });
                std::vector<glm::ivec3> ring;
                for (int x = 9; x <= 13; x++)
                    for (int z = 10; z <= 14; z++)
                        if (x == 9 || x == 13 || z == 10 || z == 14) ring.push_back({ x, 0, z });
                for (const auto& p : ring) a.client.PlaceBlock(p, BlockType::STONE, 12);
                Check(both(10.0, [&] {
                          for (const auto& p : ring) {
                              Block b;
                              if (!a.world.GetBlock(p.x, p.y, p.z, b) || b.type != BlockType::STONE || b.IsFullBlock()) return false;
                          }
                          return true;
                      }),
                      "a pen of stone slabs, three quarters high");
                server.QueueConsoleCommand("summon deer 11.5 0 11.5");
                both(10.0, [&] { return a.Find(CreatureType::Deer) != nullptr; });
                bool embedded = false;
                both(15.0, [&] {
                    if (const auto* d = a.Find(CreatureType::Deer))
                        for (const auto& p : ring)
                            if (d->feet.x > p.x + 0.1f && d->feet.x < p.x + 0.9f && d->feet.z > p.z + 0.1f && d->feet.z < p.z + 0.9f && d->feet.y < 0.7f) embedded = true;
                    return embedded;
                });
                Check(a.Find(CreatureType::Deer) && !embedded, "a deer wandering in it never walks into the slabs");
                server.QueueConsoleCommand("creatures clear");
                server.QueueConsoleCommand("fill 9 0 10 13 0 14 air");
                both(20.0, [&] { return a.client.GetCreatures().empty(); });
            }

            std::cout << "Bats" << std::endl;
            server.QueueConsoleCommand("summon bat 12.5 6 12.5");
            Check(both(20.0, [&] { return a.Find(CreatureType::Bat) != nullptr; }), "a bat is summoned in mid air");
            both(3.0, [] { return false; });
            Check(a.Find(CreatureType::Bat) && a.Find(CreatureType::Bat)->feet.y > 1.0f, "and flies about rather than falling (at " +
                                                                                          std::to_string(a.Find(CreatureType::Bat) ? a.Find(CreatureType::Bat)->feet.y : -1.0f) + ")");
            server.QueueConsoleCommand("creatures clear");
            both(20.0, [&] { return a.client.GetCreatures().empty(); });

        }
        if (sections.Run("allies")) {
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
            a.heldBlock = ItemType::RAW_VENISON;
            both(0.3, [] { return false; });
            a.client.Interact(wolf);
            Check(both(20.0, [&] { const auto* w = a.Find(CreatureType::Wolf); return w && w->tame; }) && Has(a, "Wolf tamed"), "fed raw venison, it's tamed (and wears a collar)");
            a.heldBlock = BlockType::AIR;
            server.QueueConsoleCommand("tp Hunter 22.5 0 8.5");
            Check(both(20.0, [&] { const auto* w = a.Find(CreatureType::Wolf); return w && glm::distance(glm::vec2(w->feet.x, w->feet.z), glm::vec2(22.5f, 8.5f)) < 5.0f; }),
                  "it follows its owner");
            // Far off down a tunnel a block wide and two high, it catches up in the tunnel,
            // not in its walls (where it would fall through the floor).
            server.QueueConsoleCommand("fill 50 -1 7 60 2 9 stone");
            server.QueueConsoleCommand("fill 50 0 8 60 1 8 air");
            server.QueueConsoleCommand("tp Hunter 55.5 0 8.5");
            Check(both(20.0, [&] {
                      const auto* w = a.Find(CreatureType::Wolf);
                      return w && glm::distance(glm::vec2(w->feet.x, w->feet.z), glm::vec2(55.5f, 8.5f)) < 3.0f;
                  }) && both(3.0, [] { return false; }) == false,
                  "far off down a narrow tunnel, it catches up");
            {
                const auto* w = a.Find(CreatureType::Wolf);
                Check(w && w->feet.y > -0.1f && w->feet.y < 0.6f && std::abs(w->feet.z - 8.5f) < 0.2f,
                      "in the tunnel, on its floor (at " + (w ? std::to_string(w->feet.x) + ", " + std::to_string(w->feet.y) + ", " + std::to_string(w->feet.z) : std::string("nowhere")) + ")");
            }
            server.QueueConsoleCommand("tp Hunter 22.5 0 8.5");
            server.QueueConsoleCommand("fill 50 -1 7 60 2 9 air");
            server.QueueConsoleCommand("fill 50 -1 7 60 -1 9 grass");
            both(20.0, [&] { const auto* w = a.Find(CreatureType::Wolf); return w && glm::distance(glm::vec2(w->feet.x, w->feet.z), glm::vec2(22.5f, 8.5f)) < 5.0f; });
            server.QueueConsoleCommand("time midnight");
            server.QueueConsoleCommand("summon dwarf 26.5 0 8.5");
            Check(both(20.0, [&] { return a.Find(CreatureType::DwarfWarrior) != nullptr; }), "a dwarf warrior turns up near them");
            Check(both(30.0, [&] { return a.Find(CreatureType::DwarfWarrior) == nullptr; }), "and the wolf sees it off");
            server.QueueConsoleCommand("creatures clear");
            both(20.0, [&] { return a.client.GetCreatures().empty(); });

            std::cout << "Markets" << std::endl;
            server.QueueConsoleCommand("gamemode survival Hunter");
            server.QueueConsoleCommand("time noon");
            server.QueueConsoleCommand("tp Hunter 8.5 0 8.5");
            server.QueueConsoleCommand("summon dwarf_trader 11.5 0 8.5");
            server.QueueConsoleCommand("give gold_ingot 1 Hunter");
            server.QueueConsoleCommand("give glass 8 Hunter");
            Check(both(20.0, [&] { return a.Find(CreatureType::DwarfTrader) != nullptr; }), "a dwarf trader is summoned beside the hunter");
            both(20.0, [&] { return a.client.GetResources(BlockType::GLASS) == 8 * Block::kUnitsPerBlock; });
            const uint16_t trader = a.Find(CreatureType::DwarfTrader)->id;
            a.client.Interact(trader);
            Check(both(20.0, [&] { return a.client.GetTradePrices().size() == Trades::Offers().size() && a.client.GetTradePrices()[0] == 1; }),
                  "right-clicking the trader brings its prices");
            a.client.Trade(trader, 0); // a gold ingot for 2 iron ingots
            Check(both(20.0, [&] { return a.client.GetResources(ItemType::IRON_INGOT) == 2 * Block::kUnitsPerBlock && a.client.GetResources(ItemType::GOLD_INGOT) == 0; }),
                  "a gold ingot buys 2 iron ingots");
            Check(both(10.0, [&] { return Has(a, "Merchant: trade with a dwarf trader"); }), "and earns Merchant");
            Check(both(20.0, [&] { return !a.client.GetTradePrices().empty() && a.client.GetTradePrices()[0] == 2; }), "and that offer's price goes up (2 gold now)");
            a.client.Trade(trader, 0);
            Check(both(20.0, [&] { return Has(a, "You need 2"); }) && a.client.GetResources(ItemType::IRON_INGOT) == 2 * Block::kUnitsPerBlock,
                  "a trade without the goods is refused");
            a.client.Trade(trader, 5); // 8 glass for a gold ingot
            Check(both(20.0, [&] { return a.client.GetResources(ItemType::GOLD_INGOT) == Block::kUnitsPerBlock && a.client.GetResources(BlockType::GLASS) == 0; }),
                  "they buy 8 glass for a gold ingot");
            Check(both(20.0, [&] { return a.client.GetTradePrices().size() > 5 && a.client.GetTradePrices()[5] == 10; }), "and want 10 glass the next time");
            server.QueueConsoleCommand("give gold_ingot 2 Hunter");
            both(20.0, [&] { return a.client.GetResources(ItemType::GOLD_INGOT) == 3 * Block::kUnitsPerBlock; });
            a.client.Trade(trader, static_cast<uint8_t>(Trades::Offers().size() - 1)); // a guard, for 3 gold
            Check(both(20.0, [&] { const auto* g = a.Find(CreatureType::DwarfGuard); return g && g->tame; }) && a.client.GetResources(ItemType::GOLD_INGOT) == 0,
                  "3 gold ingots hire a guard (wearing a band)");
            server.QueueConsoleCommand("tp Hunter 22.5 0 8.5");
            Check(both(20.0, [&] { const auto* g = a.Find(CreatureType::DwarfGuard); return g && glm::distance(glm::vec2(g->feet.x, g->feet.z), glm::vec2(22.5f, 8.5f)) < 5.0f; }),
                  "the guard follows whoever hired them");
            server.QueueConsoleCommand("time midnight");
            server.QueueConsoleCommand("gamemode creative Hunter");
            server.QueueConsoleCommand("summon dwarf 27.5 0 8.5");
            Check(both(20.0, [&] { return a.Find(CreatureType::DwarfWarrior) != nullptr; }), "a dwarf warrior turns up");
            Check(both(30.0, [&] { return a.Find(CreatureType::DwarfWarrior) == nullptr; }), "and the guard cuts it down");
            // Hit a market's dwarf, and the market won't deal with you.
            server.QueueConsoleCommand("gamemode survival Hunter");
            if (const auto* t = a.Find(CreatureType::DwarfTrader)) {
                char tp[96];
                std::snprintf(tp, sizeof tp, "tp Hunter %.2f 0 %.2f", t->feet.x - 1.5f, t->feet.z);
                server.QueueConsoleCommand(tp);
            }
            both(20.0, [&] {
                const auto* t = a.Find(CreatureType::DwarfTrader);
                return t && !a.client.IsCreative() && glm::distance(glm::vec2(t->feet.x, t->feet.z), glm::vec2(a.feet.x, a.feet.z)) < 2.5f;
            });
            a.client.Attack(trader, Tool::None);
            both(1.0, [] { return false; });
            a.client.Trade(trader, 1);
            Check(both(20.0, [&] { return Has(a, "won't deal with you"); }), "hit, the trader won't trade with you");
            server.QueueConsoleCommand("gamemode survival Hunter");
            server.QueueConsoleCommand("creatures clear");
            both(20.0, [&] { return a.client.GetCreatures().empty(); });

            std::cout << "The Dwarf King" << std::endl;
            server.QueueConsoleCommand("gamemode creative Hunter");
            server.QueueConsoleCommand("tp Hunter 8.5 0 8.5");
            server.QueueConsoleCommand("summon dwarf_king 10.5 0 8.5");
            Check(both(20.0, [&] { const auto* k = a.Find(CreatureType::DwarfKing); return k && k->health > 0.99f; }), "a Dwarf King is summoned, at full health");
            const uint16_t king = a.Find(CreatureType::DwarfKing)->id;
            a.client.Attack(king, Tool::Sword);
            Check(both(20.0, [&] { const auto* k = a.Find(CreatureType::DwarfKing); return k && k->health < 0.99f; }), "a hit shows on his health (for the health bar)");
            // Hit after hit (keeping by him) until he falls.
            bool slain = both(90.0, [&] {
                const auto* k = a.Find(CreatureType::DwarfKing);
                if (!k) return true;
                if (glm::distance(a.feet, k->feet) > 2.5f) {
                    char tp[96];
                    std::snprintf(tp, sizeof tp, "tp Hunter %.2f %.2f %.2f", k->feet.x - 1.5f, k->feet.y, k->feet.z);
                    server.QueueConsoleCommand(tp);
                }
                a.client.Attack(king, Tool::Sword);
                return false;
            });
            Check(slain && both(10.0, [&] { return Has(a, "Hoard: ") && Has(b, "has defeated a Dwarf King"); }), "he falls at last: his hoard is the hunter's, and everyone's told");
            server.QueueConsoleCommand("creatures clear");
            both(20.0, [&] { return a.client.GetCreatures().empty(); });
            server.QueueConsoleCommand("gamemode survival Hunter");

            std::cout << "The forge golem" << std::endl;
            server.QueueConsoleCommand("gamemode creative Hunter");
            server.QueueConsoleCommand("tp Hunter 8.5 0 8.5");
            server.QueueConsoleCommand("summon forge_golem 10.5 0 8.5");
            Check(both(20.0, [&] { return a.Find(CreatureType::ForgeGolem) != nullptr; }), "a forge golem is summoned");
            const uint16_t golem = a.Find(CreatureType::ForgeGolem)->id;
            const bool golemDown = both(90.0, [&] {
                const auto* k = a.Find(CreatureType::ForgeGolem);
                if (!k) return true;
                if (glm::distance(a.feet, k->feet) > 2.5f) {
                    char tp[96];
                    std::snprintf(tp, sizeof tp, "tp Hunter %.2f %.2f %.2f", k->feet.x - 1.6f, k->feet.y, k->feet.z);
                    server.QueueConsoleCommand(tp);
                }
                a.client.Attack(golem, Tool::Sword);
                return false;
            });
            Check(golemDown && both(10.0, [&] { return Has(a, "forge core") && a.client.GetResources(ItemType::FORGE_CORE) == Block::kUnitsPerBlock; }),
                  "it falls, leaving its forge core");
            server.QueueConsoleCommand("creatures clear");
            both(20.0, [&] { return a.client.GetCreatures().empty(); });

            std::cout << "Bosses that fall to other hands" << std::endl;
            {
                // A pit of spikes walled in obsidian. A forge golem the hunter has hit,
                // finished by the spikes, still gives them its forge core; one nobody hit
                // leaves it where it fell.
                server.QueueConsoleCommand("fill 19 0 19 23 3 23 obsidian");
                server.QueueConsoleCommand("fill 20 0 20 22 3 22 air");
                server.QueueConsoleCommand("fill 20 0 20 22 0 22 spikes");
                server.QueueConsoleCommand("tp Hunter 18.5 0 21.5");
                both(10.0, [&] { return std::abs(a.feet.x - 18.5f) < 0.1f; });
                const uint32_t cores = a.client.GetResources(ItemType::FORGE_CORE);
                server.QueueConsoleCommand("summon forge_golem 21.5 0 21.5");
                Check(both(20.0, [&] { return a.Find(CreatureType::ForgeGolem) != nullptr; }), "a forge golem in a pit of spikes");
                a.client.Attack(a.Find(CreatureType::ForgeGolem)->id, Tool::Sword);
                Check(both(60.0, [&] { return a.Find(CreatureType::ForgeGolem) == nullptr; }) &&
                          both(5.0, [&] { return a.client.GetResources(ItemType::FORGE_CORE) == cores + Block::kUnitsPerBlock; }),
                      "hit once by the hunter and finished by the spikes, its forge core is still theirs");
                server.QueueConsoleCommand("tp Hunter 8.5 0 8.5"); // (out of reach of what it leaves)
                both(10.0, [&] { return std::abs(a.feet.x - 8.5f) < 0.1f; });
                server.QueueConsoleCommand("summon forge_golem 21.5 0 21.5");
                Check(both(20.0, [&] { return a.Find(CreatureType::ForgeGolem) != nullptr; }), "another, that nobody hits");
                Check(both(60.0, [&] { return a.Find(CreatureType::ForgeGolem) == nullptr; }) && both(5.0, [&] {
                          for (const auto& i : a.client.GetItems())
                              if (i.item == Items::Of(ItemType::FORGE_CORE) && glm::distance(glm::vec2(i.position.x, i.position.z), glm::vec2(21.5f)) < 3.0f) return true;
                          return false;
                      }) && Has(a, "its forge core lies where it fell"),
                      "falls to the spikes and leaves its forge core where it fell");
                server.QueueConsoleCommand("fill 19 0 19 23 3 23 air");
            }

            std::cout << "Coming back" << std::endl;
            {
                // A third player tames a wolf, drops off (their connection goes quiet) and
                // comes back on a new connection, with a new id: the wolf still follows them.
                TestClient t;
                t.client.Connect("127.0.0.1", server.GetPort(), "Tamer", error, kTestViewDistance);
                auto withTamer = [&](TestClient& c, double s, const std::function<bool()>& done) { return Pump({ &a, &b, &c }, s, done); };
                Check(withTamer(t, 20.0, [&] { return t.joined; }), "a third player joins");
                server.QueueConsoleCommand("tp Tamer 50.5 0 8.5");
                server.QueueConsoleCommand("summon wolf 52.5 0 8.5");
                server.QueueConsoleCommand("give raw_venison 1 Tamer");
                withTamer(t, 20.0, [&] { return t.Find(CreatureType::Wolf) && t.client.GetResources(ItemType::RAW_VENISON) > 0; });
                const auto* found = t.Find(CreatureType::Wolf);
                const uint16_t theirs = found ? found->id : 0;
                t.heldBlock = ItemType::RAW_VENISON;
                withTamer(t, 0.3, [] { return false; });
                t.client.Interact(theirs);
                Check(withTamer(t, 20.0, [&] { const auto* w = t.Find(CreatureType::Wolf); return w && w->tame; }), "tames a wolf");
                wait(4.0); // (their connection quiet, as when it drops)
                TestClient back;
                back.client.Connect("127.0.0.1", server.GetPort(), "Tamer", error, kTestViewDistance);
                Check(withTamer(back, 20.0, [&] { return back.joined; }), "and comes back on a new connection");
                server.QueueConsoleCommand("tp Tamer 64.5 0 8.5");
                Check(withTamer(back, 30.0, [&] {
                          const auto* w = back.Find(CreatureType::Wolf);
                          return w && glm::distance(glm::vec2(w->feet.x, w->feet.z), glm::vec2(64.5f, 8.5f)) < 5.0f;
                      }),
                      "their wolf still follows them");
                back.client.Disconnect();
                t.client.Disconnect();
                withTamer(back, 0.5, [] { return false; });
            }
            server.QueueConsoleCommand("creatures clear");
            both(20.0, [&] { return a.client.GetCreatures().empty(); });

        }
        if (sections.Run("raids")) {
            std::cout << "Raids" << std::endl;
            // A bed in a plank hut; the hunter (creative, so left alone) inside with it.
            server.QueueConsoleCommand("gamemode creative Hunter");
            server.QueueConsoleCommand("time noon");
            server.QueueConsoleCommand("fill 30 0 30 36 3 36 planks");
            server.QueueConsoleCommand("fill 31 0 31 35 2 35 air");
            server.QueueConsoleCommand("fill 33 0 33 33 0 33 bed");
            server.QueueConsoleCommand("tp Hunter 32.5 0 32.5");
            auto planks = [&] {
                int n = 0;
                for (int x = 30; x <= 36; x++)
                    for (int z = 30; z <= 36; z++)
                        for (int y = 0; y <= 1; y++) {
                            Block blk;
                            if (a.world.GetBlock(x, y, z, blk) && blk.type == BlockType::PLANKS) n++;
                        }
                return n;
            };
            both(20.0, [&] { return planks() > 40 && glm::distance(a.feet, glm::vec3(32.5f, 0.0f, 32.5f)) < 0.5f; });
            const int wall = planks();
            a.client.UseBed({ 33, 0, 33 });
            both(5.0, [&] { return Has(a, "respawn"); });
            server.QueueConsoleCommand("raid Hunter");
            Check(both(20.0, [&] { return Has(a, "a dwarf raid is coming for your home"); }), "/raid sends a war band for the hunter's bed (and they're warned)");
            auto raiders = [&] {
                int n = 0;
                for (const auto& c : a.client.GetCreatures()) n += c.type == CreatureType::DwarfWarrior || c.type == CreatureType::DwarfCrossbow;
                return n;
            };
            Check(both(10.0, [&] { return raiders() >= 3; }), "a band of dwarves turns up");
            Check(both(90.0, [&] { return planks() < wall; }), "they hack through the hut's plank walls");
            Check(both(60.0, [&] {
                      for (const auto& c : a.client.GetCreatures())
                          if (glm::distance(glm::vec2(c.feet.x, c.feet.z), glm::vec2(33.5f, 33.5f)) < 3.5f) return true;
                      return false;
                  }),
                  "and get in to the bed");
            server.QueueConsoleCommand("time midnight");
            wait(1.0);
            server.QueueConsoleCommand("time noon");
            Check(both(20.0, [&] { return raiders() == 0; }), "at dawn the raiders go");
            server.QueueConsoleCommand("gamemode survival Hunter");

            server.QueueConsoleCommand("time midnight");
            server.QueueConsoleCommand("summon dwarf 20 0 20");
            server.QueueConsoleCommand("summon deer 22 0 20");
            server.QueueConsoleCommand("creatures");
            wait(1.0);
            server.QueueConsoleCommand("creatures clear");
            Check(both(20.0, [&] { return a.client.GetCreatures().empty(); }), "/creatures clear removes them all");
        }
        a.client.Disconnect();
        b.client.Disconnect();
        Pump({ &a, &b }, 0.3, [] { return false; });
        server.Stop();
    }

    if (sections.Run("pvp")) {
        std::cout << "PvP" << std::endl;
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
        Check(both(5.0, [&] { return b.client.GetResources(BlockType::STONE) == 0 && Has(b, "in a grave where you died"); }),
              "keeping none of their stone: it's all left where they died");
        glm::ivec3 grave(0);
        for (const auto& line : b.client.GetChat())
            if (const size_t at = line.text.find("died ("); at != std::string::npos)
                std::sscanf(line.text.c_str() + at + 6, "%d, %d, %d", &grave.x, &grave.y, &grave.z);
        Block left;
        Check(both(20.0, [&] { return a.world.GetBlock(grave.x, grave.y, grave.z, left) && left.type == BlockType::GRAVE; }),
              "in a grave the killer can see");
        server.QueueConsoleCommand("tp Duelist " + std::to_string(grave.x - 1.5f) + " 0 " + std::to_string(grave.z + 0.5f));
        both(20.0, [&] { return std::abs(a.feet.x - (grave.x - 1.5f)) < 0.1f; });
        a.client.OpenChest(grave);
        Check(both(20.0, [&] { return a.client.GetChestUnits(BlockType::STONE) == 4 * 64; }), "and open, with all 4 stone in it");
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
        Check(both(20.0, [&] { return a.client.HasTool(Tool::Bow) && a.client.GetResources(ItemType::ARROW) == 3 * 64 && std::abs(b.feet.x - 30.5f) < 0.1f; }),
              "the duelist has a bow and three arrows; the rival stands ten blocks off");
        both(3.5, [] { return false; }); // (no grace left; healed or not)
        const int beforeArrow = b.client.GetHealth();
        // Aim at their middle, a little high (arrows drop).
        const glm::vec3 eye = a.feet + glm::vec3(0.0f, 1.62f, 0.0f), target = b.feet + glm::vec3(0.0f, 1.0f + 0.5f, 0.0f);
        a.client.Shoot(target - eye);
        Check(both(5.0, [&] { return b.client.GetHealth() < beforeArrow; }) && b.client.GetHealth() == beforeArrow - 3,
              "an arrow across ten blocks hits (2.5 hearts, armour takes it to 1.5)");
        Check(a.client.GetResources(ItemType::ARROW) == 2 * 64, "and uses an arrow");
        a.client.Shoot(glm::vec3(1.0f, -1.0f, 0.0f)); // (too soon: the bow isn't ready)
        both(1.0, [] { return false; });
        Check(a.client.GetResources(ItemType::ARROW) == 2 * 64, "a second shot straight after doesn't go");
        a.client.Shoot(glm::vec3(1.0f, -1.0f, 0.0f)); // at the ground ahead
        bool stuck = false;
        both(5.0, [&] {
            for (const auto& arrow : a.client.GetArrows()) stuck |= glm::length(arrow.velocity) < 1e-3f;
            return stuck;
        });
        Check(stuck && a.client.GetResources(ItemType::ARROW) == 64, "one shot at the ground sticks there");

        // A crossbow: a bolt hits harder. Then a shield (blocking, crouched) and a steel plate.
        server.QueueConsoleCommand("give crossbow 1 Duelist");
        server.QueueConsoleCommand("give arrow 4 Duelist");
        a.heldTool = Tool::None;
        a.heldBlock = ItemType::CROSSBOW;
        both(20.0, [&] { return a.client.GetResources(ItemType::CROSSBOW) > 0 && a.client.GetResources(ItemType::ARROW) >= 4 * 64; });
        auto bolt = [&] {
            both(1.5, [] { return false; });
            const int before = b.client.GetHealth();
            const glm::vec3 from = a.feet + glm::vec3(0.0f, 1.62f, 0.0f), at = b.feet + glm::vec3(0.0f, 1.2f, 0.0f);
            a.client.Shoot(at - from);
            both(5.0, [&] { return b.client.GetHealth() < before; });
            return before - b.client.GetHealth();
        };
        const int boltHit = bolt();
        Check(boltHit == 5, "a crossbow bolt hits harder: 4.5 hearts, armour takes it to 2.5 (" + std::to_string(boltHit) + ")");
        server.QueueConsoleCommand("give planks 2 Rival");
        server.QueueConsoleCommand("give iron_ingot 1 Rival");
        both(20.0, [&] { return b.client.GetResources(ItemType::IRON_INGOT) >= 64; });
        b.client.Craft(Crafting::Find("shield"), 1);
        Check(both(20.0, [&] { return (rivalAsSeen().armour & Armour::Bit(ArmourPiece::Shield)) != 0; }), "the rival makes a shield (others see it)");
        b.crouching = true;
        const int blocked = bolt();
        Check(blocked == 2, "crouched behind it, a bolt does half: 1 heart (" + std::to_string(blocked) + ")");
        b.crouching = false;
        server.QueueConsoleCommand("give steel_ingot 4 Rival");
        server.QueueConsoleCommand("give hide 2 Rival");
        both(20.0, [&] { return b.client.GetResources(ItemType::STEEL_INGOT) >= 4 * 64; });
        b.client.Craft(Crafting::Find("plate"), 1);
        // (Worn in place of the tunic: put on, the tunic comes off into a slot.)
        both(20.0, [&] { return b.client.GetCount(Items::OfArmour(ArmourPiece::Plate)) == 1; });
        for (int i = 0; i < static_cast<int>(b.client.GetInventory().slots.size()); i++)
            if (b.client.GetInventory().slots[i].item == Items::OfArmour(ArmourPiece::Plate)) b.client.MoveItems(net::Box::Player, i, net::Box::Armour, net::kAnySlot);
        Check(both(20.0, [&] { return (rivalAsSeen().armour & Armour::Bit(ArmourPiece::Plate)) != 0 && !(rivalAsSeen().armour & Armour::Bit(ArmourPiece::Tunic)); }) &&
                  b.client.GetCount(Items::OfArmour(ArmourPiece::Tunic)) == 1,
              "and a steel plate, worn on the body in place of the tunic (which comes off into a slot)");
        const int plated = bolt();
        Check(plated == 2, "with cap, plate, boots and shield on (76% off), a bolt does 1 heart (" + std::to_string(plated) + ")");
        a.heldBlock = BlockType::AIR;
        a.client.Disconnect();
        b.client.Disconnect();
        Pump({ &a, &b }, 0.3, [] { return false; });
        server.Stop();
    }

    if (sections.Run("spawning")) {
        std::cout << "Natural spawning" << std::endl;
        Server server;
        Server::Options spawningOptions = BaseOptions(TerrainGenerator::Type::Normal);
        spawningOptions.hordeOdds = 1; // (every night a horde night, but the first)
        if (!server.Start(spawningOptions)) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Wanderer", error, kTestViewDistance);
        Check(Pump({ &a }, 30.0, [&] { return a.joined; }), "a player joins a normal world (forest at spawn)");
        server.QueueConsoleCommand("gamemode creative Wanderer");
        server.QueueConsoleCommand("time noon");
        Check(Pump({ &a }, 60.0, [&] { return a.Find(CreatureType::Deer) != nullptr; }), "deer turn up by themselves around the player");
        {
            // The world's first night is quiet: no horde (even when every other night is
            // one), no raids. The second brings the horde.
            server.QueueConsoleCommand("time midnight");
            Pump({ &a }, 3.0, [] { return false; });
            const bool quietFirst = !Has(a, "horde is coming");
            server.QueueConsoleCommand("time noon");
            Pump({ &a }, 1.0, [] { return false; });
            server.QueueConsoleCommand("time midnight");
            Check(quietFirst && Pump({ &a }, 10.0, [&] { return Has(a, "horde is coming"); }), "the first night is quiet (no horde, no raids); the second brings the horde");
            server.QueueConsoleCommand("horde off");
            server.QueueConsoleCommand("time noon");
            Pump({ &a }, 2.0, [] { return false; });
        }
        {
            // A horde night: the war band comes over the ground under the open sky, never
            // up out of the caves below (the terrain here is hills, with caves under them).
            server.QueueConsoleCommand("time midnight");
            server.QueueConsoleCommand("horde on");
            auto openSky = [&](const glm::vec3& feet) {
                const glm::ivec3 f(glm::floor(feet + glm::vec3(0.0f, 0.1f, 0.0f)));
                for (int y = f.y + 2; y < f.y + 64; y++) {
                    Block b;
                    if (!a.world.GetBlock(f.x, y, f.z, b)) return true;
                    if (!b.IsPassable() && !IsLeavesType(b.type)) return false;
                }
                return true;
            };
            std::vector<uint16_t> seen;
            int underground = 0;
            Pump({ &a }, 40.0, [&] {
                for (const auto& c : a.client.GetCreatures()) {
                    if (c.type != CreatureType::DwarfWarrior && c.type != CreatureType::DwarfCrossbow) continue;
                    if (std::find(seen.begin(), seen.end(), c.id) != seen.end()) continue;
                    seen.push_back(c.id);
                    if (!openSky(c.feet)) underground++;
                }
                return seen.size() >= 6;
            });
            Check(!seen.empty() && underground == 0,
                  "on a horde night the war band comes over the open ground (" + std::to_string(seen.size()) + " seen, " + std::to_string(underground) + " under cover)");
            server.QueueConsoleCommand("horde off");
            server.QueueConsoleCommand("creatures clear");
            server.QueueConsoleCommand("time noon");
        }
        // The nearest dwarf camp or mine: dwarves come out there when a player's near.
        const TerrainGenerator terrain(TerrainGenerator::Type::Normal);
        std::optional<Dwarves::Site> site;
        for (int ring = 0; ring < 12 && !site; ring++)
            for (int rx = -ring; rx <= ring && !site; rx++)
                for (int rz = -ring; rz <= ring && !site; rz++) {
                    if (std::max(std::abs(rx), std::abs(rz)) != ring) continue;
                    const auto r = Dwarves::SiteIn(terrain, rx, rz);
                    if (r.exists) site = r.site;
                }
        Check(site.has_value(), "there's a dwarf camp or mine within a few regions of spawn");
        Check(Pump({ &a }, 20.0, [&] { return !a.client.sites.empty(); }), "the server tells the player the dwarf sites around them (for the map)");
        if (site) {
            const glm::ivec3 c = site->centre;
            server.QueueConsoleCommand("tp Wanderer " + std::to_string(c.x + 20) + " " + std::to_string(c.y + 4) + " " + std::to_string(c.z));
            Check(Pump({ &a }, 60.0, [&] {
                      for (const auto& cr : a.client.GetCreatures())
                          if (SpecOf(cr.type).hostile && glm::distance(glm::vec2(cr.feet.x, cr.feet.z), glm::vec2(c.x, c.z)) < 30.0f) return true;
                      return false;
                  }),
                  std::string("dwarves come out at the ") + (site->kind == Dwarves::SiteKind::Camp ? "camp" : "mine") + " when a player comes near");
        }
        // Down in the Deep (a cavern floor), rock grubs come out of the dark.
        server.QueueConsoleCommand("tp Wanderer -14.5 -150 -30.5");
        Check(Pump({ &a }, 60.0, [&] { return a.Find(CreatureType::RockGrub) != nullptr; }), "in the Deep, rock grubs crawl out of the dark");
        a.client.Disconnect();
        Pump({ &a }, 0.3, [] { return false; });
        server.Stop();
    }

    if (!sections.Valid()) failures++;
    std::cout << (failures ? "CREATURE TEST FAILED (" : "ALL CREATURE CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
