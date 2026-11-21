#include "client/Client.h"

#include "world/Crafting.h"

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
    myName = name;
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
        food = m.food;
        maxFood = m.foodMax;
        if (m.hurt) lastHurt = Now();
        if (m.hurt && m.health == m.max) lastDeath = Now(); // (hurt, yet full: died and came back)
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
        tools = m.tools;
        armour = m.armour;
        break;
    }
    case net::MsgType::Abilities: {
        net::Abilities m;
        if ((ok = net::Decode(in, m))) abilities = m.flags;
        break;
    }
    case net::MsgType::TradePrices: {
        net::TradePrices m;
        if ((ok = net::Decode(in, m))) tradePrices = m.give;
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
            // A solid thing gone, or one appeared (not water moving about).
            Block old;
            if (world.GetBlock(p.x, p.y, p.z, old) && blockEvents.size() < 64) {
                const bool wasSolid = !old.IsAir() && !old.IsLiquid(), isSolid = !cell.block.IsAir() && !cell.block.IsLiquid();
                if (wasSolid && !isSolid) blockEvents.push_back({ p, old.type, false });
                else if (isSolid && cell.block.type != old.type) blockEvents.push_back({ p, cell.block.type, true });
            }
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
        storm = m.storm != 0;
        const double now = Now();
        std::unordered_map<uint16_t, std::deque<Sample>> next;
        for (const auto& p : m.players) {
            if (p.id == playerId) continue;
            auto& samples = next[p.id];
            if (auto it = remote.find(p.id); it != remote.end()) samples = std::move(it->second);
            samples.push_back({ now, { p.id, p.feet, p.yaw, p.pitch, p.flags, p.heldTool, p.heldBlock, p.armour, {} } });
            while (samples.size() > 2 && samples[1].time < now - 1.0) samples.pop_front();
        }
        remote = std::move(next); // players missing from the snapshot have left
        break;
    }
    case net::MsgType::ChestContents: {
        net::ChestContents m;
        if (!(ok = net::Decode(in, m))) break;
        chestHeld.fill(0);
        if (m.open) {
            openChest = m.pos;
            for (const auto& [type, units] : m.held) chestHeld[static_cast<size_t>(type)] = units;
        }
        else openChest.reset();
        break;
    }
    case net::MsgType::SignText: {
        net::SignText m;
        if (!(ok = net::Decode(in, m))) break;
        if (m.text.empty()) signs.erase(m.pos);
        else signs[m.pos] = m.text;
        break;
    }
    case net::MsgType::Bobber: {
        net::Bobber m;
        if (!(ok = net::Decode(in, m))) break;
        if (m.out) bobber = m.pos;
        else bobber.reset();
        bite = bite || m.bite;
        break;
    }
    case net::MsgType::Arrows: {
        net::Arrows m;
        if (!(ok = net::Decode(in, m))) break;
        arrows = std::move(m.arrows);
        arrowsAt = Now();
        break;
    }
    case net::MsgType::SignalInfo: {
        net::SignalInfo m;
        if (!(ok = net::Decode(in, m))) break;
        if (m.erased) signalInfos.erase(m.pos);
        else signalInfos[m.pos] = m;
        break;
    }
    case net::MsgType::Machines: {
        net::Machines m;
        if ((ok = net::Decode(in, m))) machines = std::move(m.machines);
        break;
    }
    case net::MsgType::Explosion: {
        net::Explosion m;
        if ((ok = net::Decode(in, m)) && explosions.size() < 32) explosions.push_back(m);
        break;
    }
    case net::MsgType::Lightning: {
        net::Lightning m;
        if ((ok = net::Decode(in, m)) && lightning.size() < 16) lightning.push_back(m.pos);
        break;
    }
    case net::MsgType::PlayerName: {
        net::PlayerName m;
        if ((ok = net::Decode(in, m))) playerNames[m.id] = m.name;
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
            const uint8_t action = (c.flags & net::kCreatureAttacking) ? 1 : (c.flags & net::kCreatureWorking) ? 2 : 0;
            samples.push_back({ now, { c.id, c.type, c.feet, c.yaw, (c.flags & net::kCreatureHurt) != 0, 0.0f, (c.flags & net::kCreatureTame) != 0, action, c.health / 255.0f } });
            while (samples.size() > 2 && samples[1].time < now - 1.0) samples.pop_front();
        }
        creatures = std::move(next); // gone from the snapshot: gone (or out of range)
        break;
    }
    case net::MsgType::Knockback: {
        net::Knockback m;
        if ((ok = net::Decode(in, m))) pendingKnockback += m.velocity;
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

void Client::SendPlayerState(const glm::vec3& feet, const glm::vec3& velocity, float yaw, float pitch, uint8_t flags, Tool heldTool,
                             BlockType heldBlock) {
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
    m.heldTool = heldTool;
    m.heldBlock = heldBlock;
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
        if (!IsCreative()) resources[static_cast<size_t>(block.ResourceType())] += static_cast<uint32_t>(block.ResourceUnits());
    }
    net.Send(net::kChannelReliable, net::Encode(net::EditBreak{ pos, tool }));
}

bool Client::CanCraft(int index, int count) const {
    const auto& recipes = Crafting::Recipes();
    if (IsCreative() || index < 0 || index >= static_cast<int>(recipes.size())) return false;
    const Recipe& recipe = recipes[index];
    if (recipe.tool != Tool::None && HasTool(recipe.tool)) return false;
    if (recipe.armour != ArmourPiece::Count && HasArmour(recipe.armour)) return false;
    if (recipe.ironUpgrade && HasIron()) return false;
    if (recipe.steelUpgrade && (HasSteel() || !HasIron())) return false;
    return Crafting::TimesAffordable(recipe, resources) >= static_cast<uint32_t>(count);
}

void Client::Craft(int index, int count) {
    if (!welcomed || !CanCraft(index, count)) return;
    // Predicted like edits; the server's answer (Resources) puts it right.
    const Recipe& recipe = Crafting::Recipes()[index];
    for (const auto& in : recipe.inputs) resources[static_cast<size_t>(in.type)] -= in.units * count;
    if (recipe.ironUpgrade) tools |= Crafting::kIronBit;
    else if (recipe.steelUpgrade) tools |= Crafting::kSteelBit;
    else if (recipe.tool != Tool::None) tools |= Crafting::ToolBit(recipe.tool);
    else if (recipe.armour != ArmourPiece::Count) armour |= Armour::Bit(recipe.armour);
    else resources[static_cast<size_t>(recipe.output)] += recipe.outputUnits * count;
    net.Send(net::kChannelReliable, net::Encode(net::Craft{ static_cast<uint8_t>(index), static_cast<uint8_t>(count) }));
}

bool Client::CanAfford(BlockType type, int heightEighths) const {
    if (IsCreative()) return true;
    const Block placed = Block::Placed(type, heightEighths);
    return !placed.IsLiquid() && GetResources(type) >= static_cast<uint32_t>(placed.ResourceUnits());
}

void Client::PlaceBlock(const glm::ivec3& pos, BlockType type, int heightEighths, int faceAxis) {
    if (!welcomed || IsItemType(type) || !CanAfford(type, heightEighths)) return; // (items aren't placed)
    const Block placed = Block::Placed(type, heightEighths, faceAxis);
    world.SetBlock(pos.x, pos.y, pos.z, placed);
    if (!IsCreative()) resources[static_cast<size_t>(ResourceTypeOf(type))] -= static_cast<uint32_t>(placed.ResourceUnits());
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
        const auto name = playerNames.find(id);
        if (i + 1 >= samples.size() || t <= samples[i].time) {
            result.push_back(samples[i].state);
            if (name != playerNames.end()) result.back().name = name->second;
            continue;
        }
        const Sample& a = samples[i];
        const Sample& b = samples[i + 1];
        const float f = static_cast<float>((t - a.time) / (b.time - a.time));
        RemotePlayer p = b.state;
        p.feet = glm::mix(a.state.feet, b.state.feet, f);
        p.yaw = LerpAngle(a.state.yaw, b.state.yaw, f);
        p.pitch = a.state.pitch + (b.state.pitch - a.state.pitch) * f;
        if (name != playerNames.end()) p.name = name->second;
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

std::vector<Client::BlockEvent> Client::TakeBlockEvents() {
    std::vector<BlockEvent> events;
    events.swap(blockEvents);
    return events;
}

void Client::WriteSign(const glm::ivec3& pos, const std::string& text) {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(net::SignText{ pos, text.substr(0, net::kMaxSignLength) }, true));
}

void Client::Cast(const glm::ivec3& pos, bool cast) {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(net::Cast{ pos, static_cast<uint8_t>(cast ? 1 : 0) }));
    if (!cast) bobber.reset();
}

void Client::Shoot(const glm::vec3& direction) {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(net::Shoot{ direction }));
}

std::vector<net::ArrowInfo> Client::GetArrows() const {
    // Moved on from the last update (smooth between the server's 20 a second).
    const float since = static_cast<float>(std::min(0.1, Now() - arrowsAt));
    std::vector<net::ArrowInfo> out = arrows;
    for (auto& a : out) {
        a.position += a.velocity * since;
        a.velocity.y -= 18.0f * since;
    }
    return out;
}

void Client::UseBed(const glm::ivec3& pos) {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(net::UseBed{ pos }));
}

void Client::OpenChest(const glm::ivec3& pos) {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(net::OpenChest{ pos, 1 }));
}

void Client::CloseChest() {
    if (welcomed && openChest) net.Send(net::kChannelReliable, net::Encode(net::OpenChest{ *openChest, 0 }));
    openChest.reset();
}

void Client::MoveChestItems(BlockType type, uint32_t units, bool intoChest) {
    if (welcomed && openChest && units > 0)
        net.Send(net::kChannelReliable, net::Encode(net::ChestMove{ *openChest, type, units, static_cast<uint8_t>(intoChest ? 1 : 0) }));
}

void Client::Eat(BlockType item) {
    if (!welcomed) return;
    net.Send(net::kChannelReliable, net::Encode(net::Eat{ item }));
}

void Client::ConfigureSignal(const net::SignalInfo& info) {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(info, true));
}

void Client::Inspect(const glm::ivec3& pos) {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(net::Inspect{ pos }));
}

void Client::Interact(uint16_t creature) {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(net::Interact{ creature }));
}

void Client::Trade(uint16_t trader, uint8_t offer) {
    if (welcomed) net.Send(net::kChannelReliable, net::Encode(net::Trade{ trader, offer }));
}

void Client::AttackPlayer(uint16_t player, Tool tool) {
    if (!welcomed) return;
    net.Send(net::kChannelReliable, net::Encode(net::AttackPlayer{ player, tool }));
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
