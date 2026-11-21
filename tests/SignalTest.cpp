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

        server.QueueConsoleCommand("save");
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
