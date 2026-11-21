// Wireless signals: levers, plates and logic gates sending on channels; lamps, valves,
// sluice gates and gates listening (see world/Signals.h).
#include <algorithm>
#include <chrono>

#include "server/Server.h"
#include "server/ServerWorld.h"

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

net::SignalInfo Server::SignalInfoAt(const glm::ivec3& pos) const {
    net::SignalInfo m;
    m.pos = pos;
    auto it = signalBlocks.find(pos);
    if (it == signalBlocks.end()) {
        m.erased = 1;
        return m;
    }
    const Signals::Config& c = it->second.config;
    m.kind = static_cast<uint8_t>(c.kind);
    m.mode = static_cast<uint8_t>(c.mode);
    m.a = c.a;
    m.b = c.b;
    m.out = c.out;
    m.on = it->second.on ? 1 : 0;
    return m;
}

void Server::BroadcastSignal(const glm::ivec3& pos) {
    const auto packet = net::Encode(SignalInfoAt(pos), false);
    for (auto& [peer, client] : clients)
        if (client.welcomed) net.Send(peer, net::kChannelReliable, packet);
}

void Server::HandleConfigureSignal(ClientState& client, const net::SignalInfo& m) {
    Block b;
    if (!InReach(client, m.pos, std::max(8.0f, EditReach(client))) || !world->GetBlock(m.pos.x, m.pos.y, m.pos.z, b)) return;
    // An open sluice gate is out of the way (air or water): it's still the same gate.
    auto existing = signalBlocks.find(m.pos);
    Signals::Kind kind = Signals::KindOf(b.type);
    if (kind == Signals::Kind::None && existing != signalBlocks.end() && existing->second.config.kind == Signals::Kind::Sluice) kind = Signals::Kind::Sluice;
    if (kind == Signals::Kind::None || static_cast<uint8_t>(kind) != m.kind) return;
    SignalBlock& s = signalBlocks[m.pos];
    s.config.kind = kind;
    s.config.mode = static_cast<Signals::Mode>(std::min<uint8_t>(m.mode, static_cast<uint8_t>(Signals::Mode::Count) - 1));
    s.config.a = Signals::Listens(kind) ? m.a : 0;
    s.config.b = kind == Signals::Kind::Gate ? m.b : 0;
    s.config.out = Signals::Sends(kind) ? m.out : 0;
    if (kind == Signals::Kind::Gate) s.on = false;
    BroadcastSignal(m.pos);
}

bool Server::CloseSluice(const glm::ivec3& pos) {
    Block cell;
    if (!world->GetBlock(pos.x, pos.y, pos.z, cell)) return false;
    if (cell.type == BlockType::SLUICE) return true;
    if (!cell.IsAir() && !cell.IsLiquid()) return false; // (something's in the way)
    int water = cell.WaterVolume();
    // The water goes up first, then to the sides, wherever there's room; only once it
    // has all gone does the gate shut (water is never lost).
    if (water > 0) {
        struct Room {
            glm::ivec3 pos;
            Block block;
            int room;
        };
        std::vector<Room> rooms;
        for (const glm::ivec3 d : { glm::ivec3(0, 1, 0), glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1) }) {
            Block n;
            const glm::ivec3 p = pos + d;
            if (!world->GetBlock(p.x, p.y, p.z, n) || !n.CanHoldWater()) continue;
            const int room = n.WaterCapacity() - n.WaterVolume();
            if (room > 0) rooms.push_back({ p, n, room });
        }
        int total = 0;
        for (const auto& r : rooms) total += r.room;
        if (total < water) return false;
        for (const auto& r : rooms) {
            if (water == 0) break;
            const int put = std::min(water, r.room);
            const Block filled = r.block.IsLiquid() || r.block.IsAir() ? Block::Water(static_cast<uint8_t>(r.block.WaterVolume() + put))
                                                                        : r.block.WithWater(static_cast<uint8_t>(r.block.WaterVolume() + put));
            world->SetBlock(r.pos.x, r.pos.y, r.pos.z, filled);
            water -= put;
        }
    }
    world->SetBlock(pos.x, pos.y, pos.z, Block(BlockType::SLUICE));
    return true;
}

void Server::UpdateSignals() {
    const double now = Now();
    if (now < nextSignalUpdate) return;
    nextSignalUpdate = now + Signals::kInterval;
    for (auto it = plateSignal.begin(); it != plateSignal.end();) it = now >= it->second ? plateSignal.erase(it) : std::next(it);
    if (signalBlocks.empty()) return;

    // What's still there (an open sluice gate is air or water in its place; anything
    // else built there replaces it). Blocks in unloaded chunks are kept, and sit out.
    std::vector<glm::ivec3> gone;
    std::vector<std::pair<glm::ivec3, Block>> loaded;
    for (const auto& [pos, s] : signalBlocks) {
        Block b;
        if (!world->GetBlock(pos.x, pos.y, pos.z, b)) continue;
        const bool openSluice = s.config.kind == Signals::Kind::Sluice && s.on && (b.IsAir() || b.IsLiquid());
        if (Signals::KindOf(b.type) != s.config.kind && !openSluice) gone.push_back(pos);
        else loaded.emplace_back(pos, b);
    }
    for (const auto& pos : gone) {
        signalBlocks.erase(pos);
        BroadcastSignal(pos);
    }

    // Who's sending, on which channel.
    struct Sender {
        glm::ivec3 pos;
        uint8_t channel;
    };
    std::vector<Sender> senders;
    for (const auto& [pos, b] : loaded) {
        const SignalBlock& s = signalBlocks[pos];
        if (s.config.out == 0) continue;
        bool sending = false;
        switch (s.config.kind) {
        case Signals::Kind::Lever: sending = b.type == BlockType::LEVER_ON; break;
        case Signals::Kind::Plate: sending = plateSignal.count(pos) > 0; break;
        case Signals::Kind::Gate: sending = s.on; break;
        default: break;
        }
        if (sending) senders.push_back({ pos, s.config.out });
    }
    const float range2 = Signals::kRange * Signals::kRange;
    auto hears = [&](uint8_t channel, const glm::ivec3& at) {
        if (channel == 0) return false;
        for (const auto& s : senders) {
            if (s.channel != channel) continue;
            const glm::vec3 d = glm::vec3(s.pos - at);
            if (glm::dot(d, d) <= range2) return true;
        }
        return false;
    };

    // Listeners follow what they hear; gates work out their next output (sent from the
    // next update, so a loop of gates runs at one step per update).
    std::vector<std::pair<glm::ivec3, bool>> gateOutputs;
    for (const auto& [pos, b] : loaded) {
        SignalBlock& s = signalBlocks[pos];
        const Signals::Config& c = s.config;
        const bool a = hears(c.a, pos);
        switch (c.kind) {
        case Signals::Kind::Gate: {
            bool next;
            if (c.mode == Signals::Mode::Clock) {
                const bool running = c.a == 0 || a;
                if (!running) next = false, s.clockAt = now;
                else if (now >= s.clockAt) next = !s.on, s.clockAt = now + Signals::kClockSeconds;
                else next = s.on;
            }
            else next = Signals::Evaluate(c.mode, a, hears(c.b, pos), s.on);
            if (next != s.on) gateOutputs.emplace_back(pos, next);
            break;
        }
        case Signals::Kind::Lamp:
            if (c.a == 0) break;
            if (a != (b.type == BlockType::SIGNAL_LAMP_ON)) world->SetBlock(pos.x, pos.y, pos.z, Block(a ? BlockType::SIGNAL_LAMP_ON : BlockType::SIGNAL_LAMP));
            break;
        case Signals::Kind::Valve:
            if (c.a == 0) break;
            if (a != (b.type == BlockType::VALVE_OPEN)) world->SetBlock(pos.x, pos.y, pos.z, Block(a ? BlockType::VALVE_OPEN : BlockType::VALVE));
            break;
        case Signals::Kind::Sluice:
            if (c.a == 0) break;
            if (a && !s.on) {
                world->SetBlock(pos.x, pos.y, pos.z, Block()); // out of the way
                s.on = true;
                BroadcastSignal(pos);
            }
            else if (!a && s.on && CloseSluice(pos)) {
                s.on = false;
                BroadcastSignal(pos);
            }
            break;
        default: break;
        }
    }
    for (const auto& [pos, on] : gateOutputs) {
        signalBlocks[pos].on = on;
        BroadcastSignal(pos);
    }
}
