// Wireless signals over a real connection (world/Signals.h). A creative player in an
// empty world sets up levers, lamps, logic gates, a valve, a sluice gate and a
// pressure plate. Checks that
//  - a lamp listening to a lever's channel lights when the lever is flipped on, and
//    goes out when it's flipped off; one on another channel doesn't,
//  - signals reach kRange blocks and no further, and a relay gate carries them on,
//  - AND, NOT and CLOCK gates do what they say,
//  - a valve on a channel opens and shuts with it, and one without opens by hand,
//  - a sluice gate on a channel opens and closes again, pushing water out of its cell,
//  - a pressure plate sends while it's stood on,
//  - a piston with air pushes a row of blocks out and pulls the first back, one
//    without air doesn't move, a chest stops it, and its head goes with it;
//  - player, water, pressure and daylight sensors send when they should,
//  - creatures press plates and die on spikes, and a player sensor set to can sense
//    them; /horde starts and ends a horde night,
//  - channels set on blocks are still there after a restart, and go when a block is
//    broken.
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/Signals.h"

namespace fs = std::filesystem;

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

bool Has(const Client& c, const std::string& text) {
    for (const auto& line : c.GetChat())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

Server::Options MakeOptions(const fs::path& dir) {
    Server::Options options;
    options.bindHost = "127.0.0.1";
    options.port = 0;
    options.viewDistance = kTestViewDistance;
    options.log = std::getenv("SIGNAL_LOG") != nullptr;
    options.worldDirectory = dir.string();
    options.worldType = TerrainGenerator::Type::Empty;
    options.weather = Server::Options::Weather::Clear;
    options.defaultMode = net::GameMode::Creative;
    options.creatures = false;
    return options;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / ("voxel_signal_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const glm::ivec3 lever1{ 8, 0, 10 }, lever2{ 9, 0, 10 };
    {
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Wirer", error, kTestViewDistance);
        auto pump = [&](double s, const std::function<bool()>& done) { return Pump(a, s, done); };
        auto wait = [&](double s) { pump(s, [] { return false; }); };
        auto blockAt = [&](const glm::ivec3& p) {
            Block b;
            return a.world.GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT;
        };
        Check(pump(20.0, [&] { return a.joined && blockAt({ 8, -1, 8 }) != BlockType::COUNT && blockAt({ 8, -1, 8 }) != BlockType::AIR; }), "a creative player joins");
        server.QueueConsoleCommand("tp Wirer 8.5 0 8.5");
        pump(20.0, [&] { return a.feet.y < 0.5f; });
        auto fill = [&](const glm::ivec3& p, const char* block) {
            server.QueueConsoleCommand("fill " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z) + " " + std::to_string(p.x) + " " +
                                       std::to_string(p.y) + " " + std::to_string(p.z) + " " + block);
        };
        auto configure = [&](const glm::ivec3& pos, Signals::Kind kind, uint8_t a_, uint8_t b_, uint8_t out, Signals::Mode mode = Signals::Mode::And) {
            net::SignalInfo info;
            info.pos = pos;
            info.kind = static_cast<uint8_t>(kind);
            info.mode = static_cast<uint8_t>(mode);
            info.a = a_;
            info.b = b_;
            info.out = out;
            a.client.ConfigureSignal(info);
            return pump(10.0, [&] {
                auto it = a.client.GetSignals().find(pos);
                return it != a.client.GetSignals().end() && it->second.a == info.a && it->second.out == info.out && it->second.mode == info.mode;
            });
        };
        auto lit = [&](const glm::ivec3& p) { return blockAt(p) == BlockType::SIGNAL_LAMP_ON; };
        // Stands somewhere (on the ground), and waits until the client has caught up.
        auto goTo = [&](float x, float z) {
            server.QueueConsoleCommand("tp Wirer " + std::to_string(x) + " 0 " + std::to_string(z));
            pump(20.0, [&] { return std::abs(a.feet.x - x) < 0.6f && std::abs(a.feet.z - z) < 0.6f; });
        };
        auto flip = [&](const glm::ivec3& lever, bool on) {
            a.client.UseBed(lever);
            return pump(10.0, [&] { return blockAt(lever) == (on ? BlockType::LEVER_ON : BlockType::LEVER); });
        };

        std::cout << "Levers and lamps" << std::endl;
        const glm::ivec3 lamp{ 12, 0, 10 }, otherLamp{ 13, 0, 10 };
        fill(lever1, "lever");
        fill(lever2, "lever");
        fill(lamp, "signal_lamp");
        fill(otherLamp, "signal_lamp");
        Check(pump(10.0, [&] { return blockAt(lever1) == BlockType::LEVER && blockAt(lamp) == BlockType::SIGNAL_LAMP && blockAt(otherLamp) == BlockType::SIGNAL_LAMP; }),
              "levers and lamps placed");
        {
            // A lever goes on the face of a solid block, pointing out of it, and only there; it
            // falls off when that block goes.
            const glm::ivec3 wall{ 16, 1, 14 }, onWall{ 17, 1, 14 }, inAir{ 17, 3, 14 };
            fill(wall, "stone");
            pump(10.0, [&] { return blockAt(wall) == BlockType::STONE; });
            Check(!a.client.PlaceBlock(inAir, BlockType::LEVER, 8, 0), "a lever can't go where nothing solid is behind it");
            Check(a.client.PlaceBlock(onWall, BlockType::LEVER, 8, 0) && pump(10.0, [&] {
                      Block b;
                      return a.world.GetBlock(onWall.x, onWall.y, onWall.z, b) && b.type == BlockType::LEVER && b.LeverFacing() == glm::ivec3(1, 0, 0);
                  }),
                  "it goes on the side of a block, pointing out of it");
            wait(0.5);
            Check(blockAt(inAir) == BlockType::AIR && blockAt(onWall) == BlockType::LEVER, "(the server agrees on both)");
            fill(wall, "air");
            Check(pump(10.0, [&] { return blockAt(onWall) == BlockType::AIR; }), "and falls off when the block it's on goes");
            Check(pump(5.0, [&] {
                      for (const auto& i : a.client.GetItems())
                          if (i.item == Items::OfBlock(BlockType::LEVER)) return true;
                      return false;
                  }),
                  "dropping the lever as an item");
        }
        Check(configure(lever1, Signals::Kind::Lever, 0, 0, 5) && configure(lamp, Signals::Kind::Lamp, 5, 0, 0) && configure(otherLamp, Signals::Kind::Lamp, 6, 0, 0),
              "a lever sends on 5, one lamp listens to 5 and another to 6");
        wait(0.5);
        Check(!lit(lamp), "with the lever off the lamp is dark");
        Check(flip(lever1, true) && pump(5.0, [&] { return lit(lamp); }), "flipping the lever on lights it");
        wait(0.5);
        Check(!lit(otherLamp), "but not the lamp on another channel");
        Check(flip(lever1, false) && pump(5.0, [&] { return !lit(lamp); }), "flipping it off puts it out");

        std::cout << "Range and relays" << std::endl;
        const glm::ivec3 farLamp{ 8 + 80, 0, 10 }, relay{ 8 + 50, 0, 10 }, relayedLamp{ 8 + 82, 0, 10 };
        goTo(48.5f, 8.5f);
        fill(farLamp, "signal_lamp");
        fill(relayedLamp, "signal_lamp");
        fill(relay, "logic_gate");
        Check(pump(10.0, [&] { return blockAt(farLamp) == BlockType::SIGNAL_LAMP && blockAt(relay) == BlockType::LOGIC_GATE; }), "a lamp 80 blocks away, and a relay halfway");
        Check(configure(farLamp, Signals::Kind::Lamp, 5, 0, 0) && configure(relayedLamp, Signals::Kind::Lamp, 7, 0, 0) &&
                  configure(relay, Signals::Kind::Gate, 5, 0, 7, Signals::Mode::Relay),
              "the far lamp listens to 5; the relay hears 5 and sends on 7, which another far lamp listens to");
        goTo(8.5f, 8.5f);
        flip(lever1, true);
        goTo(48.5f, 8.5f);
        Check(pump(5.0, [&] { return lit(relayedLamp); }), "through the relay the signal reaches 82 blocks");
        Check(!lit(farLamp), "but straight from the lever it doesn't reach 80");
        goTo(8.5f, 8.5f);
        flip(lever1, false);
        goTo(48.5f, 8.5f);
        Check(pump(5.0, [&] { return !lit(relayedLamp); }), "and the relayed lamp goes out with the lever");
        goTo(8.5f, 12.5f);

        std::cout << "Logic" << std::endl;
        const glm::ivec3 andGate{ 8, 0, 14 }, andLamp{ 10, 0, 14 }, notGate{ 8, 0, 16 }, notLamp{ 10, 0, 16 }, clock{ 8, 0, 18 }, clockLamp{ 10, 0, 18 };
        for (const auto& p : { andGate, notGate, clock }) fill(p, "logic_gate");
        for (const auto& p : { andLamp, notLamp, clockLamp }) fill(p, "signal_lamp");
        pump(10.0, [&] { return blockAt(clockLamp) == BlockType::SIGNAL_LAMP; });
        Check(configure(lever2, Signals::Kind::Lever, 0, 0, 2) && configure(andGate, Signals::Kind::Gate, 5, 2, 3, Signals::Mode::And) &&
                  configure(andLamp, Signals::Kind::Lamp, 3, 0, 0) && configure(notGate, Signals::Kind::Gate, 5, 0, 4, Signals::Mode::Not) &&
                  configure(notLamp, Signals::Kind::Lamp, 4, 0, 0),
              "an AND gate (5 and 2 to 3) and a NOT gate (not 5 to 4), each with a lamp");
        Check(pump(5.0, [&] { return lit(notLamp) && !lit(andLamp); }), "with both levers off: NOT is on, AND is off");
        flip(lever1, true);
        Check(pump(5.0, [&] { return !lit(notLamp); }) && !lit(andLamp), "one lever on: NOT goes off, AND stays off");
        flip(lever2, true);
        Check(pump(5.0, [&] { return lit(andLamp); }), "both on: AND comes on");
        flip(lever1, false);
        Check(pump(5.0, [&] { return !lit(andLamp) && lit(notLamp); }), "one off again: AND goes off, NOT comes on");
        goTo(8.5f, 17.5f);
        Check(configure(clock, Signals::Kind::Gate, 0, 0, 8, Signals::Mode::Clock) && configure(clockLamp, Signals::Kind::Lamp, 8, 0, 0), "a clock sends on 8");
        bool sawOn = false, sawOff = false;
        pump(4.0, [&] {
            (lit(clockLamp) ? sawOn : sawOff) = true;
            return sawOn && sawOff && lit(clockLamp);
        });
        Check(sawOn && sawOff, "and its lamp blinks");
        const glm::ivec3 latch{ 8, 0, 19 }, latchLamp{ 10, 0, 19 };
        fill(latch, "logic_gate");
        fill(latchLamp, "signal_lamp");
        pump(10.0, [&] { return blockAt(latchLamp) == BlockType::SIGNAL_LAMP; });
        goTo(8.5f, 12.5f);
        Check(configure(latch, Signals::Kind::Gate, 5, 2, 10, Signals::Mode::Latch) && configure(latchLamp, Signals::Kind::Lamp, 10, 0, 0),
              "a latch (set by 5, reset by 2) sends on 10");
        flip(lever2, false);
        wait(0.5);
        Check(!lit(latchLamp), "it starts off");
        flip(lever1, true);
        Check(pump(5.0, [&] { return lit(latchLamp); }), "5 sets it");
        flip(lever1, false);
        wait(0.5);
        Check(lit(latchLamp), "and it stays on after 5 goes off");
        flip(lever2, true);
        Check(pump(5.0, [&] { return !lit(latchLamp); }), "2 resets it");
        flip(lever1, true);
        wait(0.5);
        Check(!lit(latchLamp), "and while 2 is on, 5 can't set it");
        flip(lever1, false);
        goTo(8.5f, 17.5f);

        std::cout << "Valves" << std::endl;
        const glm::ivec3 valve{ 8, 0, 20 }, handValve{ 9, 0, 20 };
        fill(valve, "valve");
        fill(handValve, "valve");
        pump(10.0, [&] { return blockAt(handValve) == BlockType::VALVE; });
        a.client.UseBed(handValve);
        Check(pump(5.0, [&] { return blockAt(handValve) == BlockType::VALVE_OPEN; }), "a valve without a channel opens by hand");
        Check(configure(valve, Signals::Kind::Valve, 2, 0, 0) && pump(5.0, [&] { return blockAt(valve) == BlockType::VALVE_OPEN; }),
              "a valve listening to 2 (on) opens");
        a.client.UseBed(valve);
        wait(0.5);
        Check(blockAt(valve) == BlockType::VALVE_OPEN && Has(a.client, "answers to channel 2"), "and won't shut by hand");
        goTo(8.5f, 12.5f);
        flip(lever2, false);
        Check(pump(5.0, [&] { return blockAt(valve) == BlockType::VALVE; }), "it shuts when 2 goes off");

        std::cout << "Sluice gates" << std::endl;
        const glm::ivec3 sluice{ 14, 0, 20 };
        goTo(11.5f, 15.5f); // (within reach of the levers, and the gate)
        fill(sluice, "sluice");
        fill({ 14, 1, 20 }, "air");
        pump(10.0, [&] { return blockAt(sluice) == BlockType::SLUICE; });
        Check(configure(sluice, Signals::Kind::Sluice, 5, 0, 0), "a sluice gate listens to 5");
        flip(lever1, true);
        Check(pump(5.0, [&] { return blockAt(sluice) == BlockType::AIR; }), "it opens when 5 comes on");
        fill(sluice, "water"); // water fills the gap
        pump(5.0, [&] { Block b; return a.world.GetBlock(sluice.x, sluice.y, sluice.z, b) && b.HasWater(); });
        flip(lever1, false);
        Check(pump(10.0, [&] { return blockAt(sluice) == BlockType::SLUICE; }), "and closes when it goes off");
        Check(pump(5.0, [&] {
                  int water = 0;
                  for (const glm::ivec3 d : { glm::ivec3(0, 1, 0), glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1) }) {
                      Block b;
                      if (a.world.GetBlock(sluice.x + d.x, sluice.y + d.y, sluice.z + d.z, b)) water += b.WaterVolume();
                  }
                  return water > 0;
              }),
              "pushing the water in its cell out beside it");

        std::cout << "Pressure plates" << std::endl;
        const glm::ivec3 plate{ 20, 0, 8 }, plateLamp{ 22, 0, 8 };
        fill(plate, "pressure_plate");
        fill(plateLamp, "signal_lamp");
        pump(10.0, [&] { return blockAt(plate) == BlockType::PRESSURE_PLATE; });
        Check(configure(plate, Signals::Kind::Plate, 0, 0, 9) && configure(plateLamp, Signals::Kind::Lamp, 9, 0, 0), "a plate sends on 9");
        server.QueueConsoleCommand("tp Wirer 20.5 0 8.5");
        Check(pump(10.0, [&] { return lit(plateLamp); }), "stepping on it lights its lamp");
        server.QueueConsoleCommand("tp Wirer 8.5 0 8.5");
        Check(pump(10.0, [&] { return !lit(plateLamp); }), "which goes out after stepping off");

        std::cout << "Breaking" << std::endl;
        a.client.BreakBlock(otherLamp);
        Check(pump(5.0, [&] { return !a.client.GetSignals().count(otherLamp); }), "a broken block's channels go");

        std::cout << "Pistons" << std::endl;
        // A piston pushing +x (as /fill places it), on an air pipe, with two stones in
        // front; another with no air.
        const glm::ivec3 lever3{ 24, 0, 24 }, piston{ 26, 0, 26 }, dry{ 26, 0, 28 };
        goTo(25.5f, 22.5f);
        fill(lever3, "lever");
        fill(piston, "piston");
        fill(piston + glm::ivec3(0, 1, 0), "air_pipe");
        fill(dry, "piston");
        for (const glm::ivec3 p : { glm::ivec3(27, 0, 26), glm::ivec3(28, 0, 26), glm::ivec3(27, 0, 28) }) fill(p, "stone");
        pump(10.0, [&] { return blockAt({ 27, 0, 28 }) == BlockType::STONE; });
        Check(configure(lever3, Signals::Kind::Lever, 0, 0, 11) && configure(piston, Signals::Kind::Piston, 11, 0, 0) && configure(dry, Signals::Kind::Piston, 11, 0, 0),
              "two pistons listen to 11");
        server.QueueConsoleCommand("pressurise 26 0 26");
        wait(0.6);
        flip(lever3, true);
        Check(pump(5.0, [&] { return blockAt({ 27, 0, 26 }) == BlockType::PISTON_HEAD && blockAt({ 28, 0, 26 }) == BlockType::STONE && blockAt({ 29, 0, 26 }) == BlockType::STONE; }),
              "with air, it pushes both stones a block on, its head behind them");
        wait(0.6);
        Check(blockAt({ 27, 0, 28 }) == BlockType::STONE && blockAt({ 28, 0, 28 }) == BlockType::AIR, "without air, the other doesn't move");
        flip(lever3, false);
        Check(pump(5.0, [&] { return blockAt({ 27, 0, 26 }) == BlockType::STONE && blockAt({ 28, 0, 26 }) == BlockType::AIR && blockAt({ 29, 0, 26 }) == BlockType::STONE; }),
              "off, it pulls back, bringing the first stone with it");
        fill({ 28, 0, 26 }, "chest");
        pump(5.0, [&] { return blockAt({ 28, 0, 26 }) == BlockType::CHEST; });
        flip(lever3, true);
        wait(1.0);
        Check(blockAt({ 27, 0, 26 }) == BlockType::STONE && blockAt({ 28, 0, 26 }) == BlockType::CHEST, "a chest in the row stops it");
        flip(lever3, false);
        a.client.BreakBlock({ 28, 0, 26 });
        fill({ 28, 0, 26 }, "air");
        server.QueueConsoleCommand("pressurise 26 0 26");
        wait(0.6);
        flip(lever3, true);
        Check(pump(5.0, [&] { return blockAt({ 27, 0, 26 }) == BlockType::PISTON_HEAD; }), "with the chest gone it pushes again");
        a.client.BreakBlock(piston);
        Check(pump(5.0, [&] { return blockAt(piston) == BlockType::AIR && blockAt({ 27, 0, 26 }) == BlockType::AIR; }), "and breaking the piston takes its head");
        flip(lever3, false);

        std::cout << "Sensors" << std::endl;
        const glm::ivec3 playerSensor{ 24, 0, 32 }, waterSensor{ 26, 0, 32 }, pressureSensor{ 28, 0, 32 }, daySensor{ 30, 0, 32 };
        fill(playerSensor, "player_sensor");
        fill(waterSensor, "water_sensor");
        fill(pressureSensor, "pressure_sensor");
        fill(pressureSensor + glm::ivec3(0, 0, 1), "air_pipe");
        fill(daySensor, "daylight_sensor");
        for (int i = 0; i < 4; i++) fill(playerSensor + glm::ivec3(2 * i, 0, -1), "signal_lamp");
        // A cell above the water sensor walled in, so water put there stays.
        server.QueueConsoleCommand("fill 25 1 31 27 1 33 stone");
        fill(waterSensor + glm::ivec3(0, 1, 0), "air");
        pump(10.0, [&] { return blockAt({ 30, 0, 31 }) == BlockType::SIGNAL_LAMP && blockAt(waterSensor + glm::ivec3(0, 1, 0)) == BlockType::AIR; });
        goTo(27.5f, 24.5f);
        Check(configure(playerSensor, Signals::Kind::PlayerSensor, 0, 3, 12) && configure(waterSensor, Signals::Kind::WaterSensor, 0, 0, 13) &&
                  configure(pressureSensor, Signals::Kind::PressureSensor, 0, 50, 14) && configure(daySensor, Signals::Kind::DaylightSensor, 0, 0, 15),
              "a player sensor (3 blocks), a water sensor, a pressure sensor (50%) and a daylight sensor, sending on 12-15");
        for (int i = 0; i < 4; i++) configure(playerSensor + glm::ivec3(2 * i, 0, -1), Signals::Kind::Lamp, static_cast<uint8_t>(12 + i), 0, 0);
        const glm::ivec3 playerLamp = playerSensor + glm::ivec3(0, 0, -1), waterLamp = waterSensor + glm::ivec3(0, 0, -1),
                         pressureLamp = pressureSensor + glm::ivec3(0, 0, -1), dayLamp = daySensor + glm::ivec3(0, 0, -1);
        server.QueueConsoleCommand("time midnight");
        wait(0.6);
        Check(!lit(playerLamp) && !lit(waterLamp) && !lit(pressureLamp) && !lit(dayLamp), "at midnight, with no one near, no water and no air, all are off");
        goTo(23.5f, 33.5f);
        Check(pump(5.0, [&] { return lit(playerLamp); }), "the player sensor comes on with a player beside it");
        goTo(27.5f, 24.5f);
        Check(pump(5.0, [&] { return !lit(playerLamp); }), "and goes off when they've gone");
        fill(waterSensor + glm::ivec3(0, 1, 0), "water");
        Check(pump(5.0, [&] { return lit(waterLamp); }), "the water sensor comes on with water on it");
        fill(waterSensor + glm::ivec3(0, 1, 0), "air");
        Check(pump(5.0, [&] { return !lit(waterLamp); }), "and goes off when it's gone");
        server.QueueConsoleCommand("pressurise 28 0 32");
        Check(pump(5.0, [&] { return lit(pressureLamp); }), "the pressure sensor comes on once its pipe has air");
        server.QueueConsoleCommand("time noon");
        Check(pump(5.0, [&] { return lit(dayLamp); }), "the daylight sensor comes on at noon");
        server.QueueConsoleCommand("fill 30 3 32 30 3 32 stone");
        Check(pump(5.0, [&] { return !lit(dayLamp); }), "and goes off with a roof over it");

        std::cout << "Creatures" << std::endl;
        // A dwarf penned on a pressure plate, with a creature-sensing player sensor and a
        // players-only one beside the pen; another penned on spikes.
        server.QueueConsoleCommand("time midnight");
        server.QueueConsoleCommand("fill 39 0 39 41 1 41 stone");
        server.QueueConsoleCommand("fill 45 0 39 47 1 41 stone");
        const glm::ivec3 penPlate{ 40, 0, 40 }, penSpikes{ 46, 0, 40 }, creatureSensor{ 40, 0, 43 }, playerOnly{ 41, 0, 43 };
        const glm::ivec3 plateLamp2{ 42, 0, 45 }, creatureLamp{ 43, 0, 45 }, playerOnlyLamp{ 44, 0, 45 };
        goTo(42.5f, 48.5f);
        fill({ 40, 1, 40 }, "air");
        fill({ 46, 1, 40 }, "air");
        fill(penPlate, "pressure_plate");
        fill(penSpikes, "spikes");
        fill(creatureSensor, "player_sensor");
        fill(playerOnly, "player_sensor");
        for (const auto& p : { plateLamp2, creatureLamp, playerOnlyLamp }) fill(p, "signal_lamp");
        pump(10.0, [&] { return blockAt(playerOnlyLamp) == BlockType::SIGNAL_LAMP && blockAt(penSpikes) == BlockType::SPIKES; });
        Check(configure(penPlate, Signals::Kind::Plate, 0, 0, 20) && configure(creatureSensor, Signals::Kind::PlayerSensor, 0, 4, 21, Signals::Mode::Or) &&
                  configure(playerOnly, Signals::Kind::PlayerSensor, 0, 4, 22) && configure(plateLamp2, Signals::Kind::Lamp, 20, 0, 0) &&
                  configure(creatureLamp, Signals::Kind::Lamp, 21, 0, 0) && configure(playerOnlyLamp, Signals::Kind::Lamp, 22, 0, 0),
              "a plate sending on 20, a player sensor that senses creatures too on 21, and a players-only one on 22, each with a lamp");
        goTo(42.5f, 56.5f);
        auto ghouls = [&] {
            int n = 0;
            for (const auto& c : a.client.GetCreatures()) n += c.type == CreatureType::DwarfWarrior;
            return n;
        };
        server.QueueConsoleCommand("summon dwarf 40.5 0 40.5");
        Check(pump(5.0, [&] { return ghouls() == 1; }), "a dwarf is penned on the plate");
        Check(pump(5.0, [&] { return lit(plateLamp2); }), "it presses the plate: its lamp lights");
        Check(pump(5.0, [&] { return lit(creatureLamp); }), "the sensor set to sense creatures sees it");
        wait(0.5);
        Check(!lit(playerOnlyLamp), "the players-only sensor doesn't");
        server.QueueConsoleCommand("summon dwarf 46.5 0 40.5");
        Check(pump(5.0, [&] { return ghouls() == 2; }), "another is penned on spikes");
        Check(pump(10.0, [&] { return ghouls() == 1; }), "the spikes kill it");

        std::cout << "Horde nights" << std::endl;
        server.QueueConsoleCommand("horde on");
        Check(pump(5.0, [&] { return Has(a.client, "a horde is coming tonight"); }), "/horde on announces a horde night");
        a.client.SendChat("/horde");
        Check(pump(5.0, [&] { return Has(a.client, "Tonight is a horde night"); }), "and /horde says so");
        server.QueueConsoleCommand("horde off");
        Check(pump(5.0, [&] { return Has(a.client, "The horde fades"); }), "/horde off ends it");

        server.QueueConsoleCommand("save");
        // A machine sensor on a chest: on while the chest holds anything.
        {
            const glm::ivec3 chest{ 4, 0, 14 }, sensor{ 4, 1, 14 }, chestLamp{ 6, 0, 14 };
            server.QueueConsoleCommand("tp Wirer 5.5 0 12.5");
            pump(10.0, [&] { return std::abs(a.feet.x - 5.5f) < 0.1f && std::abs(a.feet.z - 12.5f) < 0.1f; });
            fill(chest, "chest");
            fill(sensor, "machine_sensor");
            fill(chestLamp, "signal_lamp");
            pump(10.0, [&] { return blockAt(sensor) == BlockType::MACHINE_SENSOR && blockAt(chestLamp) == BlockType::SIGNAL_LAMP; });
            Check(configure(sensor, Signals::Kind::MachineSensor, 0, 0, 12, static_cast<Signals::Mode>(Signals::Watch::HasItems)) &&
                      configure(chestLamp, Signals::Kind::Lamp, 12, 0, 0),
                  "a machine sensor on a chest, watching for anything in it, sends on 12");
            wait(1.0);
            Check(!lit(chestLamp), "with the chest empty, its lamp is dark");
            server.QueueConsoleCommand("give stone 1 Wirer");
            pump(5.0, [&] { return a.client.GetResources(BlockType::STONE) >= Block::kUnitsPerBlock; });
            a.client.OpenChest(chest);
            pump(5.0, [&] { return a.client.GetOpenChest() == chest; });
            a.client.MoveChestItems(BlockType::STONE, 1, true);
            Check(pump(5.0, [&] { return lit(chestLamp); }), "a stone in the chest lights it");
            a.client.MoveChestItems(BlockType::STONE, 1, false);
            Check(pump(5.0, [&] { return !lit(chestLamp); }), "taken out again, it goes dark");
            a.client.CloseChest();
        }
        wait(1.0);
        a.client.Disconnect();
        Pump(a, 0.3, [] { return false; });
        server.Stop();
    }
    {
        Server server;
        if (!server.Start(MakeOptions(dir))) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Wirer", error, kTestViewDistance);
        Check(Pump(a, 20.0, [&] {
                  auto it = a.client.GetSignals().find(lever1);
                  return it != a.client.GetSignals().end() && it->second.out == 5;
              }),
              "after a restart the lever still sends on 5");
        a.client.Disconnect();
        Pump(a, 0.3, [] { return false; });
        server.Stop();
    }
    fs::remove_all(dir);
    std::cout << (failures ? "SIGNAL TEST FAILED (" : "ALL SIGNAL CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
