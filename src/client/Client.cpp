#include "client/Client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

#include "client/ClientWorld.h"

namespace {

float LerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    if (d < -180.0f) d += 360.0f;
    return a + d * t;
}

} // namespace

Client::Client(ClientWorld& world)
    : world(world), blocks(std::make_unique<net::ChunkBlocks>()), light(std::make_unique<net::ChunkLight>()) {}

Client::~Client() {
    Disconnect();
}

double Client::Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool Client::Connect(const std::string& host, uint16_t port, const std::string& name, std::string& error, int viewDistance) {
    if (!net.Connect(host, port, 5000, error)) return false;
    disconnectReason.clear();
    net::Hello hello;
    hello.name = name;
    hello.viewDistance = static_cast<uint8_t>(std::clamp(viewDistance, 0, 255));
    net.Send(net::kChannelReliable, net::Encode(hello));
    net.Flush();
    return true;
}

void Client::Disconnect() {
    net.Disconnect();
    welcomed = false;
}

void Client::Poll() {
    net::NetEvent event;
    while (net.Poll(event, 0)) {
        switch (event.type) {
        case net::NetEvent::Type::Receive:
            HandleMessage(event.data);
            break;
        case net::NetEvent::Type::Disconnect:
            welcomed = false;
            if (disconnectReason.empty()) disconnectReason = "connection to the server lost";
            std::cerr << "Disconnected: " << disconnectReason << std::endl;
            break;
        case net::NetEvent::Type::Connect:
            break;
        }
    }
    net.Flush();
}

void Client::HandleMessage(const std::vector<uint8_t>& data) {
    if (data.empty()) return;
    net::ByteReader in(data.data() + 1, data.size() - 1);
    bool ok = true;
    switch (static_cast<net::MsgType>(data[0])) {
    case net::MsgType::Welcome: {
        net::Welcome m;
        if (!(ok = net::Decode(in, m))) break;
        welcomed = true;
        playerId = m.playerId;
        spawn = m.spawn;
        spawnFlags = m.flags;
        spawnYaw = m.yaw;
        spawnPitch = m.pitch;
        world.SetViewDistance(m.renderDistance, m.verticalRenderDistance);
        break;
    }
    case net::MsgType::Health: {
        net::Health m;
        if (!(ok = net::Decode(in, m))) break;
        health = m.health;
        maxHealth = m.max;
        air = m.air;
        maxAir = m.airMax;
        airCause = static_cast<Breathing::Cause>(m.airCause);
        if (m.hurt) lastHurt = Now();
        break;
    }
    case net::MsgType::GameModeIs: {
        net::GameModeIs m;
        if ((ok = net::Decode(in, m))) mode = m.mode;
        break;
    }
    case net::MsgType::Resources: {
        net::Resources m;
        if (!(ok = net::Decode(in, m))) break;
        resources.fill(0);
        for (const auto& [type, units] : m.held)
            if (static_cast<size_t>(type) < resources.size()) resources[static_cast<size_t>(type)] = units;
        break;
    }
    case net::MsgType::Abilities: {
        net::Abilities m;
        if ((ok = net::Decode(in, m))) abilities = m.flags;
        break;
    }
    case net::MsgType::ChatMessage: {
        net::ChatMessage m;
        if (!(ok = net::Decode(in, m))) break;
        chat.push_back({ m.kind, m.text, Now() });
        while (chat.size() > kMaxChatLines) chat.pop_front();
        break;
    }
    case net::MsgType::Kick: {
        net::Kick m;
        if ((ok = net::Decode(in, m))) disconnectReason = "kicked: " + m.reason;
        break;
    }
    case net::MsgType::ChunkData: {
        glm::ivec3 coords;
        if (!(ok = net::DecodeChunkData(in, coords, *blocks, *light))) break;
        world.ApplyChunk(coords, *blocks, *light);
        stats.chunksReceived++;
        break;
    }
    case net::MsgType::ChunkUnload: {
        net::ChunkUnload m;
        if ((ok = net::Decode(in, m))) world.UnloadChunk(m.coords);
        break;
    }
    case net::MsgType::LightData: {
        glm::ivec3 coords;
        if (!(ok = net::DecodeLightData(in, coords, *light))) break;
        world.ApplyLight(coords, *light);
        stats.lightUpdates++;
        break;
    }
    case net::MsgType::BlockDeltas: {
        net::BlockDeltas m;
        if (!(ok = net::Decode(in, m))) break;
        const glm::ivec3 origin = m.coords * Chunk::CHUNK_SIZE;
        constexpr int S = Chunk::CHUNK_SIZE;
        for (const auto& cell : m.cells) {
            const glm::ivec3 p = origin + glm::ivec3(cell.index % S, (cell.index / S) % S, cell.index / (S * S));
            world.SetBlock(p.x, p.y, p.z, cell.block);
        }
        stats.deltasReceived += static_cast<uint32_t>(m.cells.size());
        break;
    }
    case net::MsgType::PlayerSnapshot: {
        net::PlayerSnapshot m;
        if (!(ok = net::Decode(in, m))) break;
        if (m.tick <= lastSnapshotTick) break; // out of order
        lastSnapshotTick = m.tick;
        timeOfDay = m.timeOfDay;
        rain = m.rain / 255.0f;
        const double now = Now();
        std::unordered_map<uint16_t, std::deque<Sample>> next;
        for (const auto& p : m.players) {
            if (p.id == playerId) continue;
            auto& samples = next[p.id];
            if (auto it = remote.find(p.id); it != remote.end()) samples = std::move(it->second);
            samples.push_back({ now, { p.id, p.feet, p.yaw, p.pitch, p.flags } });
            while (samples.size() > 2 && samples[1].time < now - 1.0) samples.pop_front();
        }
        remote = std::move(next); // players missing from the snapshot have left
        break;
    }
    case net::MsgType::CreatureSnapshot: {
        net::CreatureSnapshot m;
        if (!(ok = net::Decode(in, m))) break;
        if (m.tick <= lastCreatureTick) break; // out of order
        lastCreatureTick = m.tick;
        const double now = Now();
        std::unordered_map<uint16_t, std::deque<CreatureSample>> next;
        for (const auto& c : m.creatures) {
            auto& samples = next[c.id];
            if (auto it = creatures.find(c.id); it != creatures.end()) samples = std::move(it->second);
            samples.push_back({ now, { c.id, c.type, c.feet, c.yaw, (c.flags & net::kCreatureHurt) != 0, 0.0f } });
            while (samples.size() > 2 && samples[1].time < now - 1.0) samples.pop_front();
        }
        creatures = std::move(next); // gone from the snapshot: gone (or out of range)
        break;
    }
    case net::MsgType::Correction: {
        net::Correction m;
        if (!(ok = net::Decode(in, m))) break;
        correctionId = m.correctionId;
        pendingCorrection = m.feet;
        break;
    }
    default:
        ok = false;
        break;
    }
    if (!ok) std::cerr << "Ignoring a malformed message from the server (type " << static_cast<int>(data[0]) << ")" << std::endl;
}

std::optional<glm::vec3> Client::TakeCorrection() {
    auto c = pendingCorrection;
    pendingCorrection.reset();
    return c;
}

void Client::SendPlayerState(const glm::vec3& feet, const glm::vec3& velocity, float yaw, float pitch, uint8_t flags) {
    const double now = Now();
    if (!welcomed || now - lastStateSent < 1.0 / kStateRate) return;
    lastStateSent = now;
    net::PlayerState m;
    m.sequence = ++stateSequence;
    m.correctionId = correctionId;
    m.feet = feet;
    m.velocity = velocity;
    m.yaw = yaw;
    m.pitch = pitch;
    m.flags = flags;
    net.Send(net::kChannelUnreliable, net::Encode(m));
}

void Client::StartMining(const glm::ivec3& pos, Tool tool) {
    if (!welcomed) return;
    net.Send(net::kChannelReliable, net::Encode(net::MineStart{ pos, tool }));
}

void Client::BreakBlock(const glm::ivec3& pos, Tool tool) {
    if (!welcomed) return;
    Block block;
    if (world.GetBlock(pos.x, pos.y, pos.z, block)) {
        // Breaking a waterlogged block leaves its water behind (as the server does).
        world.SetBlock(pos.x, pos.y, pos.z, block.HasWater() ? Block::Water(block.WaterVolume()) : Block());
        if (!IsCreative()) resources[static_cast<size_t>(block.type)] += static_cast<uint32_t>(block.ResourceUnits());
    }
    net.Send(net::kChannelReliable, net::Encode(net::EditBreak{ pos, tool }));
}

bool Client::CanAfford(BlockType type, int heightEighths) const {
    if (IsCreative()) return true;
    const Block placed = Block::Placed(type, heightEighths);
    return !placed.IsLiquid() && GetResources(type) >= static_cast<uint32_t>(placed.ResourceUnits());
}

void Client::PlaceBlock(const glm::ivec3& pos, BlockType type, int heightEighths, int faceAxis) {
    if (!welcomed || !CanAfford(type, heightEighths)) return;
    const Block placed = Block::Placed(type, heightEighths, faceAxis);
    world.SetBlock(pos.x, pos.y, pos.z, placed);
    if (!IsCreative()) resources[static_cast<size_t>(type)] -= static_cast<uint32_t>(placed.ResourceUnits());
    net.Send(net::kChannelReliable, net::Encode(net::EditPlace{ pos, type, static_cast<uint8_t>(heightEighths), static_cast<uint8_t>(faceAxis) }));
}

void Client::Chisel(const glm::ivec3& pos, int edge, bool raise) {
    if (!welcomed || edge < 0 || edge > 3) return;
    Block block;
    if (!world.GetBlock(pos.x, pos.y, pos.z, block) || !block.CanChisel()) return;
    const Block after = block.Chiselled(edge, raise ? 2 : -2);
    if (!IsCreative()) {
        uint32_t& held = resources[static_cast<size_t>(block.type)];
        const int change = after.ResourceUnits() - block.ResourceUnits();
        if (change > 0 && held < static_cast<uint32_t>(change)) return; // can't afford to build it up
        held = static_cast<uint32_t>(static_cast<int64_t>(held) - change);
    }
    world.SetBlock(pos.x, pos.y, pos.z, after);
    net.Send(net::kChannelReliable, net::Encode(net::EditChisel{ pos, static_cast<uint8_t>(edge), static_cast<uint8_t>(raise ? 1 : 0) }));
}

void Client::SendChat(const std::string& text) {
    if (welcomed && !text.empty()) net.Send(net::kChannelReliable, net::Encode(net::ChatSend{ text.substr(0, net::kMaxChatLength) }));
}

void Client::SkipTime() {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(net::SkipTime{}));
}

void Client::Carve(const std::vector<glm::ivec3>& centers, int radius) {
    if (!welcomed || centers.empty()) return;
    radius = std::clamp(radius, 0, net::EditCarve::kMaxRadius);
    const int r = radius;
    for (const auto& c : centers)
        for (int x = -r; x <= r; x++)
            for (int y = -r; y <= r; y++)
                for (int z = -r; z <= r; z++)
                    if (x * x + y * y + z * z <= r * r) world.SetBlock(c.x + x, c.y + y, c.z + z, Block());

    for (size_t start = 0; start < centers.size(); start += net::EditCarve::kMaxCenters) {
        net::EditCarve m;
        m.radius = static_cast<uint8_t>(radius);
        m.centers.assign(centers.begin() + start, centers.begin() + std::min(centers.size(), start + net::EditCarve::kMaxCenters));
        net.Send(net::kChannelReliable, net::Encode(m));
    }
}

std::vector<Client::RemotePlayer> Client::GetRemotePlayers() const {
    const double t = Now() - kInterpolationDelay;
    std::vector<RemotePlayer> result;
    for (const auto& [id, samples] : remote) {
        if (samples.empty()) continue;
        // The two samples around t; before the first or after the last, hold it.
        size_t i = 0;
        while (i + 1 < samples.size() && samples[i + 1].time <= t) i++;
        if (i + 1 >= samples.size() || t <= samples[i].time) {
            result.push_back(samples[i].state);
            continue;
        }
        const Sample& a = samples[i];
        const Sample& b = samples[i + 1];
        const float f = static_cast<float>((t - a.time) / (b.time - a.time));
        RemotePlayer p = b.state;
        p.feet = glm::mix(a.state.feet, b.state.feet, f);
        p.yaw = LerpAngle(a.state.yaw, b.state.yaw, f);
        p.pitch = a.state.pitch + (b.state.pitch - a.state.pitch) * f;
        result.push_back(p);
    }
    return result;
}

std::vector<Client::Creature> Client::GetCreatures() const {
    const double t = Now() - kInterpolationDelay;
    std::vector<Creature> result;
    for (const auto& [id, samples] : creatures) {
        if (samples.empty()) continue;
        size_t i = 0;
        while (i + 1 < samples.size() && samples[i + 1].time <= t) i++;
        if (i + 1 >= samples.size() || t <= samples[i].time) {
            result.push_back(samples[i].state);
            continue;
        }
        const CreatureSample& a = samples[i];
        const CreatureSample& b = samples[i + 1];
        const float f = static_cast<float>((t - a.time) / (b.time - a.time));
        Creature c = b.state;
        c.feet = glm::mix(a.state.feet, b.state.feet, f);
        c.yaw = LerpAngle(a.state.yaw, b.state.yaw, f);
        const glm::vec3 d = b.state.feet - a.state.feet;
        c.speed = static_cast<float>(glm::length(glm::vec2(d.x, d.z)) / std::max(1e-3, b.time - a.time));
        result.push_back(c);
    }
    return result;
}

void Client::Attack(uint16_t creature, Tool tool) {
    if (!welcomed) return;
    net.Send(net::kChannelReliable, net::Encode(net::Attack{ creature, tool }));
}

Client::Stats Client::GetStats() const {
    Stats s = stats;
    s.roundTripMs = net.GetStats().roundTripMs;
    s.bytesReceived = net.GetBytesReceived();
    return s;
}
