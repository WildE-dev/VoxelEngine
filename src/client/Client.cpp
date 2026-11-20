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

bool Client::Connect(const std::string& host, uint16_t port, const std::string& name, std::string& error) {
    if (!net.Connect(host, port, 5000, error)) return false;
    disconnectReason.clear();
    net::Hello hello;
    hello.name = name;
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

void Client::BreakBlock(const glm::ivec3& pos) {
    if (!welcomed) return;
    Block block;
    if (world.GetBlock(pos.x, pos.y, pos.z, block)) {
        // Breaking a waterlogged block leaves its water behind (as the server does).
        world.SetBlock(pos.x, pos.y, pos.z, block.HasWater() ? Block::Water(block.WaterVolume()) : Block());
    }
    net.Send(net::kChannelReliable, net::Encode(net::EditBreak{ pos }));
}

void Client::PlaceBlock(const glm::ivec3& pos, BlockType type) {
    if (!welcomed) return;
    world.SetBlock(pos.x, pos.y, pos.z, type == BlockType::WATER ? Block::Water(Block::kMaxWaterVolume) : Block(type));
    net.Send(net::kChannelReliable, net::Encode(net::EditPlace{ pos, type }));
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

Client::Stats Client::GetStats() const {
    Stats s = stats;
    s.roundTripMs = net.GetStats().roundTripMs;
    s.bytesReceived = net.GetBytesReceived();
    return s;
}
