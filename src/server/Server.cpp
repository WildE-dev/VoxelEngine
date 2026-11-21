#include "server/Server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>

#include "server/ServerWorld.h"
#include "server/WorldStorage.h"
#include "world/Collision.h"
#include "world/Dungeons.h"
#include "world/Noise.h"

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// World data sent per client per tick (~10 MB/s at 20 Hz). Over a slow link ENet
// queues what doesn't go out; kMaxQueuedBytes stops us piling more on top.
constexpr size_t kBytesPerTick = 512 * 1024;
constexpr size_t kMaxQueuedBytes = 4 * 1024 * 1024;
// A chunk with more changed cells than this is re-sent whole (smaller, once RLE'd).
constexpr size_t kResendThreshold = 400;

// Loose movement validation. The fastest legitimate movement is falling at terminal
// velocity (50 blocks/s); fly mode sprints at 20.
constexpr float kMaxSpeed = 55.0f;
constexpr float kMoveSlack = 1.5f;
constexpr int kBlockedStatesBeforeCorrection = 4;

// Body size, matching Player (app/Player.h).
constexpr float kPlayerWidth = 0.6f;
constexpr float kPlayerHeight = 1.8f;
constexpr float kEyeHeight = 1.62f;

// Edit reach, matching the client's raycast (50) and laser (100 + radius), plus slack
// for movement while the request is in flight.
constexpr float kReach = 56.0f;
constexpr float kLaserReach = 112.0f;
// Survival players reach 6 blocks (the client's raycast), plus slack; chests open
// from that close in any mode.
constexpr float kSurvivalReach = 7.5f;
constexpr float kChestReach = kSurvivalReach;
// Players count as asleep for this long after getting into bed (so everyone can get in).
constexpr double kSleepWindow = 6.0;
// Falls up to this many blocks are safe; each block beyond costs half a heart.
constexpr float kSafeFall = 3.0f;
// Arrows: how fast they leave the bow, how fast they fall, what a hit does, and how
// often a bow can shoot.
constexpr float kArrowSpeed = 32.0f, kArrowGravity = 18.0f;
constexpr int kArrowDamage = 5;
constexpr double kShotInterval = 0.6;
constexpr uint16_t kCreatureShooter = 0xFFFF; // an arrow a skeleton shot
// TNT: seconds from lighting to the bang, and how far the blast reaches.
constexpr double kTntFuse = 3.0;
constexpr int kTntRadius = 3;

// A player's body; a crouching player's is shorter (net::kFlagCrouching).
constexpr float kCrouchHeight = 1.45f; // as Player::kCrouchHeight
Collision::AABB BodyAt(const glm::vec3& feet, float shrink = 0.0f, bool crouching = false) {
    const float half = kPlayerWidth * 0.5f - shrink;
    const float height = crouching ? kCrouchHeight : kPlayerHeight;
    return { feet - glm::vec3(half, -shrink, half), feet + glm::vec3(half, height - shrink, half) };
}

uint16_t LocalIndex(const glm::ivec3& world) {
    const glm::ivec3 l = Coords::LocalOf(world);
    return static_cast<uint16_t>(Chunk::Index(l.x, l.y, l.z));
}

glm::ivec3 LocalFromIndex(uint16_t index) {
    constexpr int S = Chunk::CHUNK_SIZE;
    return { index % S, (index / S) % S, index / (S * S) };
}

int DistanceSquared(const glm::ivec3& a, const glm::ivec3& b) {
    const glm::ivec3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

// Whether a body overlaps a loaded full block. Unloaded space doesn't count here (unlike
// Collision::BoxBlocked), so a player isn't punished for chunks the server dropped.
bool InsideSolid(const ServerWorld& world, const Collision::AABB& box) {
    const glm::ivec3 lo = glm::ivec3(glm::floor(box.min));
    const glm::ivec3 hi = glm::ivec3(glm::floor(box.max));
    for (int x = lo.x; x <= hi.x; x++)
        for (int y = lo.y; y <= hi.y; y++)
            for (int z = lo.z; z <= hi.z; z++) {
                Block block;
                if (world.GetBlock(x, y, z, block) && block.IsFullBlock()) return true;
            }
    return false;
}

} // namespace

Server::Server()
    : blockScratch(std::make_unique<net::ChunkBlocks>()), lightScratch(std::make_unique<net::ChunkLight>()) {
    const int r = ServerWorld::RENDER_DISTANCE;
    const int v = ServerWorld::VERTICAL_RENDER_DISTANCE;
    for (int x = -r; x <= r; x++)
        for (int y = -v; y < v; y++)
            for (int z = -r; z <= r; z++) loadOffsets.emplace_back(x, y, z);
    std::sort(loadOffsets.begin(), loadOffsets.end(),
              [](const glm::ivec3& a, const glm::ivec3& b) { return DistanceSquared(a, {}) < DistanceSquared(b, {}); });
}

Server::~Server() {
    Stop();
    for (auto& [peer, client] : clients) net.Disconnect(peer);
    net.Flush();
}

bool Server::Start(const Options& opts) {
    options = opts;
    if (!net.Listen(options.bindHost, options.port, kMaxClients)) {
        std::cerr << "[server] could not listen on " << options.bindHost << ":" << options.port << std::endl;
        return false;
    }
    if (!options.worldDirectory.empty()) {
        storage = std::make_unique<WorldStorage>(options.worldDirectory);
        if (!storage->Open()) {
            std::cerr << "[server] could not open the world in " << options.worldDirectory << std::endl;
            storage.reset();
            return false;
        }
        if (storage->IsNew()) {
            storage->SetWorldType(static_cast<uint8_t>(options.worldType));
            storage->SetDefaultMode(static_cast<uint8_t>(options.defaultMode));
        }
        else if (storage->GetWorldType() > static_cast<uint8_t>(TerrainGenerator::kLastType)) {
            std::cerr << "[server] " << options.worldDirectory << " uses a world type this version doesn't know" << std::endl;
            storage.reset();
            return false;
        }
        else if (storage->GetWorldType() != static_cast<uint8_t>(options.worldType)) {
            Log("note: the existing world in " + options.worldDirectory + " keeps its own world type; use another --world for a new one");
        }
    }
    terrain.SetType(storage ? static_cast<TerrainGenerator::Type>(storage->GetWorldType()) : options.worldType);
    creatures.SetNaturalSpawning(options.creatures && terrain.GetType() == TerrainGenerator::Type::Normal);
    if (storage) timeOfDay = storage->GetTimeOfDay();
    defaultMode = storage ? static_cast<net::GameMode>(storage->GetDefaultMode()) : options.defaultMode;
    LoadOperators();
    for (const auto& name : options.operators) SetOperator(name, true);
    creatures.SetShooter([this](const glm::vec3& from, const glm::vec3& dir) {
        arrows.push_back({ from, dir * kArrowSpeed * 0.8f, kCreatureShooter, Now() });
    });
    if (storage) {
        for (const auto& [pos, held] : storage->GetChests()) chests[pos] = held;
        for (const auto& pos : storage->GetCrops()) crops.insert(pos);
        for (const auto& [pos, text] : storage->GetSigns()) signs[pos] = text;
        for (const auto& sig : storage->GetSignals()) {
            SignalBlock& b = signalBlocks[sig.pos];
            b.config.kind = static_cast<Signals::Kind>(std::min<uint8_t>(sig.kind, static_cast<uint8_t>(Signals::Kind::Count) - 1));
            b.config.mode = static_cast<Signals::Mode>(std::min<uint8_t>(sig.mode, static_cast<uint8_t>(Signals::Mode::Count) - 1));
            b.config.a = sig.a;
            b.config.b = sig.b;
            b.config.out = sig.out;
            b.on = sig.on != 0;
        }
    }
    weatherSeed ^= static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    weatherMode = options.weather;
    raining = options.weather == Options::Weather::Rain || options.weather == Options::Weather::Storm;
    storm = options.weather == Options::Weather::Storm;
    rainLevel = raining ? 1.0f : 0.0f;
    weatherTimer = 10.0 * 60.0; // the first shower comes after a while
    if (storage) {
        Log("world: " + storage->GetDirectory().string() + " (" + std::string(terrain.GetType() == TerrainGenerator::Type::Empty ? "empty, " : "") +
            std::to_string(storage->GetStoredChunkCount()) + " saved chunks)");
    }
    else if (terrain.GetType() == TerrainGenerator::Type::Empty) {
        Log("world: empty, not saved");
    }
    world = std::make_unique<ServerWorld>(terrain, storage.get(), options.viewDistance);
    world->SetWaterSimulationDistance(options.waterSimulationDistance);
    world->StartWaterThread();
    startTime = Now();
    lastSaveTime = startTime;
    Log("water simulated within " + std::to_string(world->GetWaterSimulationDistance()) + " chunks of players (view distance " +
        std::to_string(world->GetRenderDistance()) + ")");
    Log("listening on " + (options.bindHost.empty() ? std::string("0.0.0.0") : options.bindHost) + ":" + std::to_string(GetPort()));
    return true;
}

uint16_t Server::GetPort() const {
    return net.GetPort();
}

void Server::Log(const std::string& message) const {
    if (options.log) std::cout << "[server] " << message << std::endl;
}

void Server::RunInBackground() {
    // Set here, not on the thread: a Stop() right after this must not be undone by the
    // thread starting late.
    running = true;
    thread = std::thread(&Server::Loop, this);
}

void Server::Stop() {
    running = false;
    if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join();
}

void Server::Run() {
    running = true;
    Loop();
}

void Server::Loop() {
    const double interval = 1.0 / kTickRate;
    double nextTick = Now();
    net::NetEvent event;
    while (running) {
        const double now = Now();
        if (now >= nextTick) {
            Tick();
            nextTick += interval;
            if (nextTick < now) nextTick = now + interval; // fell behind: don't try to catch up
            continue;
        }
        const auto waitMs = static_cast<uint32_t>(std::clamp((nextTick - now) * 1000.0, 0.0, 50.0));
        if (net.Poll(event, waitMs)) HandleEvent(event);
    }
    Save(true);
}

void Server::StorePlayer(const ClientState& client) {
    if (!storage || !client.welcomed || !client.hasState) return;
    WorldStorage::PlayerRecord record;
    record.feet = client.feet;
    record.yaw = client.yaw;
    record.pitch = client.pitch;
    record.flags = static_cast<uint8_t>(client.noclip ? client.flags : client.flags & ~net::kFlagFlying);
    record.mode = static_cast<uint8_t>(client.mode);
    record.resources = client.resources;
    record.tools = client.tools;
    record.health = client.health;
    record.food = client.food;
    record.armour = client.armour;
    record.bed = client.bed;
    record.achievements = client.achievements;
    storage->StorePlayer(client.name, record);
}

void Server::Save(bool shuttingDown) {
    if (!storage || !world) return;
    // Nothing may move once the final save is taken.
    if (shuttingDown) world->GetWater().Stop();
    const double start = Now();
    for (const auto& [peer, client] : clients) StorePlayer(client);
    storage->SetTimeOfDay(static_cast<float>(timeOfDay));
    storage->SetChests({ chests.begin(), chests.end() });
    storage->SetCrops({ crops.begin(), crops.end() });
    storage->SetSigns({ signs.begin(), signs.end() });
    std::vector<WorldStorage::SavedSignal> savedSignals;
    for (const auto& [pos, b] : signalBlocks)
        savedSignals.push_back({ pos, static_cast<uint8_t>(b.config.kind), static_cast<uint8_t>(b.config.mode), b.config.a, b.config.b, b.config.out,
                                 static_cast<uint8_t>(b.on ? 1 : 0) });
    storage->SetSignals(std::move(savedSignals));
    const size_t saved = world->SaveAll();
    const bool ok = storage->Flush();
    lastSaveTime = Now();
    if (shuttingDown || saved > 0) {
        char ms[32];
        std::snprintf(ms, sizeof ms, "%.0f ms", (lastSaveTime - start) * 1000.0);
        Log(std::string(ok ? "saved " : "save FAILED after ") + std::to_string(saved) + " chunks (" +
            std::to_string(storage->GetStoredChunkCount()) + " stored) in " + ms);
    }
}

// --- Events -----------------------------------------------------------------------

void Server::HandleEvent(const net::NetEvent& event) {
    switch (event.type) {
    case net::NetEvent::Type::Connect:
        clients[event.peer].peer = event.peer;
        break;
    case net::NetEvent::Type::Disconnect: {
        auto it = clients.find(event.peer);
        if (it == clients.end()) break;
        const bool announce = it->second.welcomed;
        const std::string name = it->second.name;
        if (announce) Log(name + " (player " + std::to_string(it->second.id) + ") left");
        StorePlayer(it->second);
        clients.erase(it);
        if (announce) Broadcast(net::ChatMessage::Kind::Info, name + " left the game");
        break;
    }
    case net::NetEvent::Type::Receive: {
        auto it = clients.find(event.peer);
        if (it != clients.end()) HandleMessage(it->second, event.data);
        break;
    }
    }
}

void Server::HandleMessage(ClientState& client, const std::vector<uint8_t>& data) {
    if (client.kicked) return;
    if (data.empty()) return Kick(client, "empty message");
    net::ByteReader in(data.data() + 1, data.size() - 1);
    const auto type = static_cast<net::MsgType>(data[0]);

    if (!client.welcomed && type != net::MsgType::Hello) return Kick(client, "expected Hello");

    bool ok = false;
    switch (type) {
    case net::MsgType::Hello: {
        net::Hello m;
        if ((ok = net::Decode(in, m) && !client.welcomed)) HandleHello(client, m);
        break;
    }
    case net::MsgType::PlayerState: {
        net::PlayerState m;
        if ((ok = net::Decode(in, m))) HandlePlayerState(client, m);
        break;
    }
    case net::MsgType::EditBreak: {
        net::EditBreak m;
        if ((ok = net::Decode(in, m))) HandleBreak(client, m.pos, m.tool);
        break;
    }
    case net::MsgType::Attack: {
        net::Attack m;
        if ((ok = net::Decode(in, m))) HandleAttack(client, m.creature, m.tool);
        break;
    }
    case net::MsgType::ConfigureSignal: {
        net::SignalInfo m;
        if ((ok = net::Decode(in, m))) HandleConfigureSignal(client, m);
        break;
    }
    case net::MsgType::Inspect: {
        net::Inspect m;
        if ((ok = net::Decode(in, m))) HandleInspect(client, m.pos);
        break;
    }
    case net::MsgType::Interact: {
        net::Interact m;
        if ((ok = net::Decode(in, m))) HandleInteract(client, m.creature);
        break;
    }
    case net::MsgType::AttackPlayer: {
        net::AttackPlayer m;
        if ((ok = net::Decode(in, m))) HandleAttackPlayer(client, m.player, m.tool);
        break;
    }
    case net::MsgType::WriteSign: {
        net::SignText m;
        if ((ok = net::Decode(in, m))) HandleWriteSign(client, m.pos, m.text);
        break;
    }
    case net::MsgType::Cast: {
        net::Cast m;
        if ((ok = net::Decode(in, m))) HandleCast(client, m.pos, m.cast != 0);
        break;
    }
    case net::MsgType::Shoot: {
        net::Shoot m;
        if ((ok = net::Decode(in, m))) HandleShoot(client, m.direction);
        break;
    }
    case net::MsgType::UseBed: {
        net::UseBed m;
        if ((ok = net::Decode(in, m))) HandleUseBed(client, m.pos);
        break;
    }
    case net::MsgType::OpenChest: {
        net::OpenChest m;
        if ((ok = net::Decode(in, m))) HandleOpenChest(client, m.pos, m.open != 0);
        break;
    }
    case net::MsgType::ChestMove: {
        net::ChestMove m;
        if ((ok = net::Decode(in, m))) HandleChestMove(client, m.pos, m.type, m.units, m.intoChest != 0);
        break;
    }
    case net::MsgType::Eat: {
        net::Eat m;
        if ((ok = net::Decode(in, m))) HandleEat(client, m.food);
        break;
    }
    case net::MsgType::Craft: {
        net::Craft m;
        if ((ok = net::Decode(in, m))) HandleCraft(client, m.recipe, m.count);
        break;
    }
    case net::MsgType::MineStart: {
        net::MineStart m;
        if ((ok = net::Decode(in, m))) HandleMineStart(client, m.pos, m.tool);
        break;
    }
    case net::MsgType::EditPlace: {
        net::EditPlace m;
        if ((ok = net::Decode(in, m))) HandlePlace(client, m.pos, m.type, m.height, m.faceAxis);
        break;
    }
    case net::MsgType::EditChisel: {
        net::EditChisel m;
        if ((ok = net::Decode(in, m))) HandleChisel(client, m);
        break;
    }
    case net::MsgType::EditCarve: {
        net::EditCarve m;
        if ((ok = net::Decode(in, m))) HandleCarve(client, m);
        break;
    }
    case net::MsgType::SkipTime:
        ok = in.AtEnd();
        if (ok) HandleSkipTime(client);
        break;
    case net::MsgType::ChatSend: {
        net::ChatSend m;
        if ((ok = net::Decode(in, m))) HandleChat(client, m.text);
        break;
    }
    default:
        break;
    }
    if (!ok) Kick(client, "malformed message");
}

void Server::Kick(ClientState& client, const std::string& reason) {
    // Players who'd joined are announced (their disconnect isn't, as they're no longer
    // welcomed once kicked); clients that never got in aren't.
    if (client.welcomed) Broadcast(net::ChatMessage::Kind::Info, client.name + " was kicked (" + reason + ")");
    else Log("kicking " + (client.name.empty() ? std::string("a client") : client.name) + ": " + reason);
    net.Send(client.peer, net::kChannelReliable, net::Encode(net::Kick{ reason }));
    net.Disconnect(client.peer);
    client.welcomed = false;
    client.kicked = true; // ignore anything else it sends
}

void Server::HandleHello(ClientState& client, const net::Hello& hello) {
    if (hello.version != net::kProtocolVersion) {
        return Kick(client, "protocol version " + std::to_string(hello.version) + " (server speaks " +
                                std::to_string(net::kProtocolVersion) + ")");
    }
    client.id = nextPlayerId++;
    client.mode = defaultMode;
    const int most = world->GetRenderDistance();
    client.viewDistance = hello.viewDistance > 0 ? std::clamp(static_cast<int>(hello.viewDistance), 2, most) : most;
    client.verticalViewDistance = std::min(world->GetVerticalRenderDistance(), std::max(client.viewDistance, 4));
    client.name = hello.name.empty() ? "Player " + std::to_string(client.id) : hello.name;
    client.welcomed = true;

    // Spread players out a little so they don't spawn inside each other. The client
    // drops onto the terrain below this point once it has loaded.
    const int slot = (client.id - 1) % 8;
    client.feet = glm::vec3(7.5f + 2.0f * slot, 7.0f, 7.5f);
    client.lastValid = client.feet;
    client.lastStateTime = Now();

    net::Welcome welcome;
    welcome.playerId = client.id;
    if (terrain.GetType() == TerrainGenerator::Type::Empty) welcome.flags |= net::kWelcomeEmptyWorld;
    welcome.spawn = client.feet;
    welcome.renderDistance = client.viewDistance;
    welcome.verticalRenderDistance = client.verticalViewDistance;

    // Rejoining a saved world: back to where they left. (Two players with the same name
    // share the record and simply both start there.)
    if (storage) {
        if (auto saved = storage->FindPlayer(client.name)) {
            client.feet = client.lastValid = saved->feet;
            client.yaw = saved->yaw;
            client.pitch = saved->pitch;
            client.mode = static_cast<net::GameMode>(saved->mode);
            client.resources = saved->resources;
            client.tools = saved->tools;
            client.flags = saved->flags;
            client.health = std::clamp(saved->health, 1, kMaxHealth);
            client.food = std::clamp(saved->food, 0, Hunger::kMaxFood);
            client.armour = saved->armour;
            client.bed = saved->bed;
            client.achievements = saved->achievements;
            welcome.spawn = saved->feet;
            welcome.yaw = saved->yaw;
            welcome.pitch = saved->pitch;
            // Still in no-clip if they left in it and are still allowed it.
            client.noclip = (saved->flags & net::kFlagFlying) && IsOperatorName(client.name);
            welcome.flags |= static_cast<uint8_t>(net::kWelcomeResume | (client.noclip ? net::kFlagFlying : 0));
        }
    }
    net.Send(client.peer, net::kChannelReliable, net::Encode(welcome));
    Log(client.name + " joined as player " + std::to_string(client.id));
    Broadcast(net::ChatMessage::Kind::Info, client.name + " joined the game");
    // Every sign's text (there aren't many).
    for (const auto& [pos, text] : signs) net.Send(client.peer, net::kChannelReliable, net::Encode(net::SignText{ pos, text }, false));
    // And every signal block's channels.
    for (const auto& [pos, b] : signalBlocks) net.Send(client.peer, net::kChannelReliable, net::Encode(SignalInfoAt(pos), false));
    // Names for name tags: theirs to everyone, everyone else's to them.
    for (const auto& [peer, other] : clients) {
        if (!other.welcomed) continue;
        net.Send(other.peer, net::kChannelReliable, net::Encode(net::PlayerName{ client.id, client.name }));
        if (other.id != client.id) net.Send(client.peer, net::kChannelReliable, net::Encode(net::PlayerName{ other.id, other.name }));
    }
    SendChat(client, net::ChatMessage::Kind::Info, "Press T to chat, / for commands (/help lists them)");
    if (IsOperator(&client)) SendChat(client, net::ChatMessage::Kind::Info, "You are an operator");
    SendAbilities(client);
    SendHealth(client, false);
    SendMode(client);
    SendResources(client);
}

void Server::HandlePlayerState(ClientState& client, const net::PlayerState& state) {
    // States sent before the client applied our last correction still carry the old
    // position: ignore them.
    if (state.correctionId != client.correctionId) return;
    if (client.hasState && state.sequence <= client.lastSequence) return;
    client.lastSequence = state.sequence;

    const double now = Now();
    const float dt = static_cast<float>(std::clamp(now - client.lastStateTime, 0.05, 2.0));
    // Only flying (through blocks) with no-clip on: otherwise the flag is ignored.
    const bool flying = (state.flags & net::kFlagFlying) && client.noclip;

    auto correct = [&](const glm::vec3& to, const char* why) {
        client.correctionId++;
        client.feet = to;
        client.blockedStates = 0;
        net.Send(client.peer, net::kChannelReliable, net::Encode(net::Correction{ client.correctionId, to }));
        Log("corrected " + client.name + ": " + why);
    };

    // The first state after joining: the client has dropped onto the terrain below its
    // spawn point, however far down that was.
    if (!client.hasState) {
        const glm::vec2 offset(state.feet.x - client.feet.x, state.feet.z - client.feet.z);
        if (glm::length(offset) > 2.0f) return correct(client.feet, "first position too far from spawn");
    }
    else if (glm::distance(state.feet, client.lastValid) > kMaxSpeed * dt + kMoveSlack) {
        return correct(client.lastValid, "moved too fast");
    }

    // Walking players can't be inside solid blocks. The client may briefly disagree
    // (a block placed where it stands, not yet received), so only a run of these counts.
    const bool blocked = !flying && InsideSolid(*world, BodyAt(state.feet, 0.05f, (state.flags & net::kFlagCrouching) != 0));
    client.blockedStates = blocked ? client.blockedStates + 1 : 0;
    if (client.blockedStates >= kBlockedStatesBeforeCorrection) return correct(client.lastValid, "inside solid blocks");

    const bool first = !client.hasState;
    client.hasState = true;
    UpdateFall(client, state, first || flying);
    client.feet = state.feet;
    client.yaw = state.yaw;
    client.pitch = state.pitch;
    client.flags = state.flags;
    client.heldTool = state.heldTool;
    client.heldBlock = state.heldBlock;
    client.lastStateTime = now;
    if (!blocked) client.lastValid = state.feet;
}

void Server::UpdateFall(ClientState& client, const net::PlayerState& state, bool reset) {
    // Falls count from the highest point since last on the ground (or in water); a
    // landing more than kSafeFall below it hurts. Teleports, respawns, flying, swimming
    // and creative start the count afresh.
    // On the ground: as the server sees it (ground just under the feet), not as the
    // client says, so a client can't skip landing.
    bool unknown = false;
    const bool grounded = Collision::GroundHeight(*world, BodyAt(state.feet, 0.05f, (state.flags & net::kFlagCrouching) != 0), 0.05f, 0.1f, unknown).has_value() || unknown;
    Block at;
    const glm::ivec3 cell(glm::floor(state.feet + glm::vec3(0.0f, 0.1f, 0.0f)));
    Block above;
    const bool swimming = (state.flags & (net::kFlagSwimming | net::kFlagBoat)) != 0 || (world->GetBlock(cell.x, cell.y, cell.z, at) && (at.HasWater() || at.type == BlockType::LADDER)) ||
                          (world->GetBlock(cell.x, cell.y + 1, cell.z, above) && above.type == BlockType::LADDER); // (a ladder ends a fall too)
    if (reset || swimming || Creative(client)) {
        client.fallPeak = state.feet.y;
        client.falling = false;
        return;
    }
    if (!grounded) {
        client.fallPeak = client.falling ? std::max(client.fallPeak, state.feet.y) : std::max(client.feet.y, state.feet.y);
        client.falling = true;
        return;
    }
    const float fall = client.falling ? client.fallPeak - state.feet.y : 0.0f;
    client.falling = false;
    client.fallPeak = state.feet.y;
    if (fall > kSafeFall + 0.5f) Hurt(client, static_cast<int>(std::lround(fall - kSafeFall)), "fell from a high place", false);
}

float Server::EditReach(const ClientState& client) const {
    return Creative(client) ? kReach : kSurvivalReach;
}

bool Server::InReach(const ClientState& client, const glm::ivec3& pos, float reach) const {
    const glm::vec3 eye = client.feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
    return glm::distance(eye, glm::vec3(pos) + glm::vec3(0.5f)) <= reach;
}

void Server::Echo(ClientState& client, const glm::ivec3& pos) {
    const glm::ivec3 chunk = Coords::ChunkOf(pos);
    if (client.sent.count(chunk)) client.pendingCells[chunk].insert(LocalIndex(pos));
}

Tool Server::Usable(const ClientState& client, Tool tool) {
    return Creative(client) || (client.tools & Crafting::ToolBit(tool)) ? tool : Tool::None;
}

void Server::HandleCraft(ClientState& client, int recipeIndex, int count) {
    const auto& recipes = Crafting::Recipes();
    if (Creative(client) || recipeIndex < 0 || recipeIndex >= static_cast<int>(recipes.size())) return;
    const Recipe& recipe = recipes[recipeIndex];
    if (recipe.MadeOnce()) {
        const bool made = recipe.ironUpgrade       ? (client.tools & Crafting::kIronBit) != 0
                          : recipe.steelUpgrade     ? (client.tools & Crafting::kSteelBit) != 0
                          : recipe.tool != Tool::None ? (client.tools & Crafting::ToolBit(recipe.tool)) != 0
                                                      : (client.armour & Armour::Bit(recipe.armour)) != 0;
        if (made) return SendResources(client); // made already
        if (recipe.steelUpgrade && !(client.tools & Crafting::kIronBit)) return SendResources(client); // iron first
        count = 1;
    }
    if (Crafting::TimesAffordable(recipe, client.resources) < static_cast<uint32_t>(count)) return SendResources(client); // can't afford
    for (const auto& in : recipe.inputs) client.resources[static_cast<size_t>(in.type)] -= in.units * count;
    if (recipe.ironUpgrade) client.tools |= Crafting::kIronBit;
    else if (recipe.steelUpgrade) client.tools |= Crafting::kSteelBit;
    else if (recipe.tool != Tool::None) client.tools |= Crafting::ToolBit(recipe.tool);
    else if (recipe.armour != ArmourPiece::Count) client.armour |= Armour::Bit(recipe.armour);
    else client.resources[static_cast<size_t>(recipe.output)] += recipe.outputUnits * count;
    if (recipe.tool != Tool::None) Award(client, Achievement::Toolmaker);
    if (recipe.ironUpgrade) Award(client, Achievement::IronAge);
    if (recipe.output == BlockType::COOKED_VENISON) Award(client, Achievement::Cook);
    if (recipe.output == BlockType::BREAD) Award(client, Achievement::Baker);
    if (client.armour == (Armour::Bit(ArmourPiece::Cap) | Armour::Bit(ArmourPiece::Tunic) | Armour::Bit(ArmourPiece::Boots))) Award(client, Achievement::Armoured);
    SendResources(client);
}

void Server::HandleAttack(ClientState& client, uint16_t creature, Tool tool) {
    tool = Usable(client, tool);
    const double now = Now();
    if (now < client.nextAttack) return;
    client.nextAttack = now + Combat::kCooldown * 0.8; // (a little slack for timing)
    bool killed = false;
    const glm::vec3 eye = client.feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
    const CreatureSystem::Creature* target = creatures.Find(creature);
    const CreatureType type = target ? target->type : CreatureType::Count;
    const bool landed = creatures.Attack(creature, eye, tool, Crafting::Tier(client.tools), now, killed);
    if (landed && !killed) creatures.Provoke(creature, client.id, now);
    if (landed && killed) {
        std::string spoils;
        // A deer gives venison (one or two).
        if (type == CreatureType::Deer) {
            const uint32_t venison = 1 + static_cast<uint32_t>(tickCount % 2);
            client.resources[static_cast<size_t>(BlockType::RAW_VENISON)] += venison * Block::kUnitsPerBlock;
            client.resources[static_cast<size_t>(BlockType::HIDE)] += Block::kUnitsPerBlock; // and its hide
            SendResources(client);
            spoils = " (+" + std::to_string(venison) + " raw venison, +1 hide)";
        }
        // A skeleton leaves some of its arrows.
        if (type == CreatureType::Skeleton) {
            client.resources[static_cast<size_t>(BlockType::ARROW)] += 2 * Block::kUnitsPerBlock;
            SendResources(client);
            spoils = " (+2 arrows)";
        }
        SendChat(client, net::ChatMessage::Kind::Info, std::string("You defeated a ") + SpecOf(type).id + spoils);
        if (type != CreatureType::Skeleton) Award(client, type == CreatureType::Deer ? Achievement::Hunter : Achievement::GhoulSlayer);
    }
}

void Server::HandleInteract(ClientState& client, uint16_t creature) {
    const CreatureSystem::Creature* c = creatures.Find(creature);
    if (!c || c->type != CreatureType::Wolf || c->owner != 0) return;
    const glm::vec3 eye = client.feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
    const Collision::AABB box = CreatureSystem::Box(*c);
    if (glm::distance(eye, glm::clamp(eye, box.min, box.max)) > Combat::kReach + 0.5f) return;
    // Raw venison in hand tames it.
    uint32_t& venison = client.resources[static_cast<size_t>(BlockType::RAW_VENISON)];
    if (client.heldBlock != BlockType::RAW_VENISON || (!Creative(client) && venison < Block::kUnitsPerBlock)) return;
    if (!creatures.Tame(creature, client.id)) return;
    if (!Creative(client)) {
        venison -= Block::kUnitsPerBlock;
        SendResources(client);
    }
    SendChat(client, net::ChatMessage::Kind::Info, "The wolf is yours: it'll follow you, and fight for you");
    Award(client, Achievement::BestFriend);
}

void Server::HandleAttackPlayer(ClientState& client, uint16_t player, Tool tool) {
    tool = Usable(client, tool);
    const double now = Now();
    if (now < client.nextAttack || player == client.id) return;
    client.nextAttack = now + Combat::kCooldown * 0.8; // (a little slack for timing)
    if (!options.pvp) return SendChat(client, net::ChatMessage::Kind::Error, "PvP is off on this server");
    for (auto& [peer, target] : clients) {
        if (target.id != player) continue;
        if (!target.welcomed || !target.hasState || (target.noclip && (target.flags & net::kFlagFlying))) return;
        // Within reach: from the hitter's eye to the target's box (with slack for
        // movement in flight).
        const glm::vec3 eye = client.feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
        const glm::vec3 lo = target.feet - glm::vec3(0.3f, 0.0f, 0.3f), hi = target.feet + glm::vec3(0.3f, 1.8f, 0.3f);
        if (glm::distance(eye, glm::clamp(eye, lo, hi)) > Combat::kReach + 0.5f) return;
        // Knocked back, away from the hitter and a little up (as creatures are). The
        // target moves itself, so it's told to; a slain player respawns instead.
        const double before = target.invulnerableUntil;
        Hurt(target, Combat::Damage(tool, Crafting::Tier(client.tools)), "was slain by " + client.name);
        if (target.health > 0 && target.lastHurt >= now && target.invulnerableUntil == before) {
            glm::vec2 away(target.feet.x - client.feet.x, target.feet.z - client.feet.z);
            away = glm::length(away) > 1e-3f ? glm::normalize(away) : glm::vec2(1.0f, 0.0f);
            net.Send(target.peer, net::kChannelReliable, net::Encode(net::Knockback{ glm::vec3(away.x * 7.0f, 5.5f, away.y * 7.0f) }));
        }
        return;
    }
}

void Server::UpdateCreatures() {
    const double now = Now();
    const float dt = lastCreatureTime > 0.0 ? static_cast<float>(std::clamp(now - lastCreatureTime, 0.0, 0.1)) : 1.0f / kTickRate;
    lastCreatureTime = now;
    std::vector<CreatureSystem::Player> players;
    for (const auto& [peer, client] : clients) {
        if (!client.welcomed || !client.hasState) continue;
        const bool flying = client.noclip && (client.flags & net::kFlagFlying);
        players.push_back({ client.id, client.feet, !Creative(client) && !flying });
    }
    // Daylight, as the client works it out: full once the sun is a little up.
    const float sun = static_cast<float>(std::sin(timeOfDay * 6.283185307179586));
    const float daylight = std::clamp((sun + 0.12f) / 0.37f, 0.0f, 1.0f);
    creatures.Update(*world, players, now, dt, daylight, [&](uint16_t playerId, int damage, const char* deathMessage) {
        for (auto& [peer, client] : clients)
            if (client.id == playerId) Hurt(client, damage, deathMessage);
    });
}

void Server::SendCreatures() {
    const double now = Now();
    constexpr float kSendRange = 72.0f;
    for (auto& [peer, client] : clients) {
        if (!client.welcomed) continue;
        net::CreatureSnapshot m;
        m.tick = tickCount;
        for (const auto& c : creatures.All()) {
            if (glm::distance(c.feet, client.feet) > kSendRange) continue;
            m.creatures.push_back({ c.id, c.type, c.feet, c.yaw,
                                    static_cast<uint8_t>((now < c.hurtUntil ? net::kCreatureHurt : 0) | (c.owner != 0 ? net::kCreatureTame : 0)) });
        }
        net.Send(peer, net::kChannelUnreliable, net::Encode(m));
    }
}

void Server::HandleMineStart(ClientState& client, const glm::ivec3& pos, Tool tool) {
    tool = Usable(client, tool);
    client.mining = InReach(client, pos, EditReach(client));
    client.miningPos = pos;
    client.miningTool = tool;
    client.miningSince = Now();
}

void Server::HandleBreak(ClientState& client, const glm::ivec3& pos, Tool tool) {
    tool = Usable(client, tool);
    Block block;
    if (!InReach(client, pos, EditReach(client)) || !world->GetBlock(pos.x, pos.y, pos.z, block) || block.IsAir() || block.IsLiquid()) {
        return Echo(client, pos);
    }
    // Survival: only once it's been mined for long enough, with that tool.
    if (!Creative(client)) {
        const double needed = Mining::Seconds(block, tool, Crafting::Tier(client.tools));
        const bool started = client.mining && client.miningPos == pos && client.miningTool == tool;
        const double mined = started ? Now() - client.miningSince : 0.0;
        if (needed > 0.0 && mined < needed * (1.0 - Mining::kSlackShare) - Mining::kSlackSeconds) {
            if (options.log && started) Log(client.name + " broke a block too soon");
            return Echo(client, pos);
        }
        client.mining = false;
    }
    // Breaking a waterlogged block leaves its water behind.
    world->SetBlock(pos.x, pos.y, pos.z, block.HasWater() ? Block::Water(block.WaterVolume()) : Block());
    if (block.type == BlockType::CHEST) RemoveChest(pos, &client);
    if (block.type == BlockType::CROP) crops.erase(pos);
    if (block.type == BlockType::SIGN && signs.erase(pos)) BroadcastSign(pos);
    // A door goes as a whole: the other half too (its worth is counted once, below).
    if (block.type == BlockType::DOOR || block.type == BlockType::DOOR_OPEN)
        for (const int dy : { 1, -1 }) {
            Block other;
            if (world->GetBlock(pos.x, pos.y + dy, pos.z, other) && other.type == block.type) {
                world->SetBlock(pos.x, pos.y + dy, pos.z, Block());
                break;
            }
        }
    if (!Creative(client)) {
        // Ripe wheat also gives seeds (one or two); tall grass now and then a seed.
        const float roll = Noise::Random01(pos.x, pos.y, pos.z, static_cast<uint32_t>(tickCount));
        uint32_t seeds = 0;
        if (block.type == BlockType::RIPE_WHEAT) seeds = roll < 0.5f ? 1 : 2;
        if (block.type == BlockType::TALL_GRASS && roll < 0.3f) seeds = 1;
        client.resources[static_cast<size_t>(BlockType::SEEDS)] += seeds * Block::kUnitsPerBlock;
        // Leaves now and then drop a sapling (a pine's, from pine needles).
        if (IsLeavesType(block.type) && roll > 0.88f)
            client.resources[static_cast<size_t>(block.type == BlockType::PINE_LEAVES ? BlockType::PINE_SAPLING : BlockType::SAPLING)] += Block::kUnitsPerBlock;
    }
    // Survival: the block's worth goes to the player.
    if (!Creative(client)) {
        Award(client, Achievement::FirstBlock);
        if (block.type == BlockType::IRON_ORE) Award(client, Achievement::Miner);
        if (block.type == BlockType::RIPE_WHEAT) Award(client, Achievement::Farmer);
        uint32_t& held = client.resources[static_cast<size_t>(block.ResourceType())]; // (pebbles are stone)
        held += static_cast<uint32_t>(block.ResourceUnits());
        SendResources(client);
    }
}

void Server::HandlePlace(ClientState& client, const glm::ivec3& pos, BlockType type, int height, int faceAxis) {
    Block existing;
    const bool valid = type != BlockType::AIR && !IsItemType(type) && InReach(client, pos, EditReach(client)) && world->GetBlock(pos.x, pos.y, pos.z, existing) &&
                       existing.IsPassable();
    if (!valid) {
        // The client took the cost when it predicted the placement: put its count right.
        if (!Creative(client)) SendResources(client);
        return Echo(client, pos);
    }

    // A door needs room for its upper half too.
    if (type == BlockType::DOOR) {
        Block upper;
        if (!world->GetBlock(pos.x, pos.y + 1, pos.z, upper) || !upper.IsPassable() || upper.type == BlockType::DOOR_OPEN) {
            if (!Creative(client)) SendResources(client);
            return Echo(client, pos);
        }
    }
    // Seeds are planted on dirt or grass.
    if (type == BlockType::CROP || type == BlockType::SAPLING || type == BlockType::PINE_SAPLING) {
        Block soil;
        if (!world->GetBlock(pos.x, pos.y - 1, pos.z, soil) || !(soil.type == BlockType::DIRT || IsGrassType(soil.type)) || !soil.IsFullBlock()) {
            if (!Creative(client)) SendResources(client);
            return Echo(client, pos);
        }
    }
    const Block placed = Block::Placed(type, height, faceAxis);
    // Survival: it costs its worth, which the player must have. Water can't be
    // collected (only moved, with pumps), so it can't be placed either.
    const uint32_t cost = static_cast<uint32_t>(placed.ResourceUnits());
    uint32_t& held = client.resources[static_cast<size_t>(ResourceTypeOf(type))];
    if (!Creative(client) && (held < cost || placed.IsLiquid())) {
        SendResources(client); // so the client's count is right again
        return Echo(client, pos);
    }
    // No solid blocks inside a walking player (water is fine).
    if (placed.IsFullBlock()) {
        const glm::vec3 lo(pos), hi = lo + glm::vec3(1.0f);
        for (const auto& [peer, other] : clients) {
            if (!other.welcomed || ((other.flags & net::kFlagFlying) && other.noclip)) continue;
            const Collision::AABB body = BodyAt(other.feet, 0.0f, (other.flags & net::kFlagCrouching) != 0);
            if (glm::all(glm::lessThan(lo, body.max)) && glm::all(glm::greaterThan(hi, body.min))) return Echo(client, pos);
        }
    }
    world->SetBlock(pos.x, pos.y, pos.z, placed);
    if (type == BlockType::CHEST) chests[pos] = ChestItems{}; // a new chest is empty
    if (type == BlockType::CROP || type == BlockType::SAPLING || type == BlockType::PINE_SAPLING) crops.insert(pos);
    if (type == BlockType::DOOR) world->SetBlock(pos.x, pos.y + 1, pos.z, Block(BlockType::DOOR)); // its upper half (part of the one door)
    if (!Creative(client)) {
        held -= cost;
        SendResources(client);
    }
}

// A quarter of a block, up or down, per chisel stroke.
constexpr int kChiselStep = 2; // eighths

void Server::HandleChisel(ClientState& client, const net::EditChisel& chisel) {
    const glm::ivec3& p = chisel.pos;
    Block block;
    if (!InReach(client, p, EditReach(client)) || !world->GetBlock(p.x, p.y, p.z, block) || !block.CanChisel()) return Echo(client, p);
    const Block after = block.Chiselled(chisel.edge, chisel.raise ? kChiselStep : -kChiselStep);
    // Survival: chiselling off gives back what it takes away; building up costs it.
    if (!Creative(client)) {
        uint32_t& held = client.resources[static_cast<size_t>(block.type)];
        const int change = after.ResourceUnits() - block.ResourceUnits(); // + costs, - gives
        if (change > 0 && held < static_cast<uint32_t>(change)) {
            SendResources(client);
            return Echo(client, p);
        }
        held = static_cast<uint32_t>(static_cast<int64_t>(held) - change);
        SendResources(client);
    }
    world->SetBlock(p.x, p.y, p.z, after);
}

void Server::HandleCarve(ClientState& client, const net::EditCarve& carve) {
    if (!IsOperator(&client)) return; // the laser is for operators (their client doesn't offer it otherwise)
    const int r = carve.radius;
    for (const auto& center : carve.centers) {
        if (!InReach(client, center, kLaserReach)) continue;
        for (int x = -r; x <= r; x++)
            for (int y = -r; y <= r; y++)
                for (int z = -r; z <= r; z++)
                    if (x * x + y * y + z * z <= r * r) {
                        const glm::ivec3 p = center + glm::ivec3(x, y, z);
                        world->SetBlock(p.x, p.y, p.z, BlockType::AIR);
                        if (chests.count(p)) RemoveChest(p, nullptr); // (lasered: its contents are gone)
                    }
    }
}

void Server::HandleSkipTime(ClientState& client) {
    // By day, on to the evening (just after dark); at night, on to the morning.
    const bool day = timeOfDay < 0.5;
    timeOfDay = day ? 0.6 : 0.1;
    Log(client.name + " skipped to " + (day ? "night" : "morning"));
}

void Server::UpdateSky() {
    const double dt = 1.0 / kTickRate;
    timeOfDay += dt / std::max(1.0f, options.dayLengthSeconds);
    timeOfDay -= std::floor(timeOfDay);

    auto randomRange = [&](double lo, double hi) {
        weatherSeed = weatherSeed * 1664525u + 1013904223u; // LCG: plenty for weather
        return lo + (hi - lo) * (weatherSeed >> 8) / double(1u << 24);
    };
    switch (weatherMode) {
    case Options::Weather::Clear: raining = storm = false; break;
    case Options::Weather::Rain: raining = true, storm = false; break;
    case Options::Weather::Storm: raining = storm = true; break;
    case Options::Weather::Auto:
        weatherTimer -= dt;
        if (weatherTimer <= 0.0) {
            raining = !raining;
            storm = raining && randomRange(0.0, 1.0) < 0.3; // three showers in ten are thunderstorms
            weatherTimer = raining ? randomRange(2.0 * 60.0, 5.0 * 60.0) : randomRange(10.0 * 60.0, 25.0 * 60.0);
            Log(raining ? (storm ? "a thunderstorm starts" : "it starts to rain") : "the rain stops");
        }
        break;
    }
    const float target = raining ? 1.0f : 0.0f;
    const float step = static_cast<float>(dt / 10.0); // fades over ten seconds
    rainLevel = rainLevel < target ? std::min(target, rainLevel + step) : std::max(target, rainLevel - step);
    world->GetWater().SetRaining(rainLevel > 0.5f);
    UpdateStorm();
}

void Server::UpdateStorm() {
    const double now = Now();
    if (!storm || rainLevel < 0.7f || now < nextLightning) return;
    std::vector<ClientState*> players;
    for (auto& [peer, client] : clients)
        if (client.welcomed && client.hasState) players.push_back(&client);
    weatherSeed = weatherSeed * 1664525u + 1013904223u;
    auto random01 = [&] {
        weatherSeed = weatherSeed * 1664525u + 1013904223u;
        return (weatherSeed >> 8) / double(1u << 24);
    };
    nextLightning = now + 3.0 + 9.0 * random01();
    if (players.empty()) return;
    // Somewhere within 40 blocks of a random player, on top of whatever's there.
    const ClientState& near = *players[static_cast<size_t>(random01() * players.size()) % players.size()];
    const double angle = random01() * 6.283185307179586, distance = 4.0 + 36.0 * random01();
    const int x = static_cast<int>(std::floor(near.feet.x + std::cos(angle) * distance)), z = static_cast<int>(std::floor(near.feet.z + std::sin(angle) * distance));
    std::optional<glm::ivec3> top;
    for (int y = static_cast<int>(near.feet.y) + 48; y > static_cast<int>(near.feet.y) - 48; y--) {
        Block b;
        if (!world->GetBlock(x, y, z, b)) continue;
        if (!b.IsAir() && !b.IsPlant()) {
            top = glm::ivec3(x, y + 1, z);
            break;
        }
    }
    if (!top) return;
    const auto packet = net::Encode(net::Lightning{ *top });
    for (auto& [peer, client] : clients)
        if (client.welcomed) net.Send(peer, net::kChannelReliable, packet);
    // Anyone close by is struck: three hearts (armour helps).
    for (ClientState* p : players)
        if (glm::distance(p->feet, glm::vec3(*top) + glm::vec3(0.5f, 0.0f, 0.5f)) < 3.0f) Hurt(*p, 6, "was struck by lightning");
}

// --- Traps and health -------------------------------------------------------------

void Server::SendMode(ClientState& client) {
    net.Send(client.peer, net::kChannelReliable, net::Encode(net::GameModeIs{ client.mode }));
}

void Server::SendResources(ClientState& client) {
    net::Resources m;
    for (size_t t = 0; t < client.resources.size(); t++)
        if (client.resources[t] > 0) m.held.emplace_back(static_cast<BlockType>(t), client.resources[t]);
    m.tools = client.tools;
    m.armour = client.armour;
    net.Send(client.peer, net::kChannelReliable, net::Encode(m));
}

glm::vec3 Server::SpawnPoint() const {
    return glm::vec3(7.5f, static_cast<float>(terrain.ColumnAt(7, 7).FirstOpenRow()) + 1.0f, 7.5f);
}

void Server::SendHealth(ClientState& client, bool hurt) {
    client.sentHealth = client.health;
    client.sentAir = static_cast<int>(std::ceil(client.air));
    client.sentAirCause = client.airCause;
    client.sentFood = client.food;
    net::Health m;
    m.food = static_cast<uint8_t>(client.food);
    m.foodMax = static_cast<uint8_t>(Hunger::kMaxFood);
    m.health = static_cast<uint8_t>(client.health);
    m.max = static_cast<uint8_t>(kMaxHealth);
    m.hurt = static_cast<uint8_t>(hurt ? 1 : 0);
    m.air = static_cast<uint8_t>(client.sentAir);
    m.airMax = static_cast<uint8_t>(Breathing::kMaxAir);
    m.airCause = static_cast<uint8_t>(client.airCause);
    net.Send(client.peer, net::kChannelReliable, net::Encode(m));
}

void Server::Hurt(ClientState& client, int amount, const std::string& deathMessage, bool armourHelps) {
    const double now = Now();
    if (now < client.invulnerableUntil || Creative(client)) return; // nothing hurts in creative
    if (armourHelps) amount = Armour::Reduce(amount, client.armour);
    client.health = std::max(0, client.health - amount);
    client.lastHurt = now;
    client.nextRegen = now + kRegenDelay;
    if (client.health > 0) return SendHealth(client, true);

    // Dead: half of what they carry is left where they died, then back to the spawn
    // point, healed, and briefly safe.
    Broadcast(net::ChatMessage::Kind::Info, client.name + " " + deathMessage);
    DropOnDeath(client);
    client.health = kMaxHealth;
    client.air = Breathing::kMaxAir;
    client.food = Hunger::kMaxFood;
    client.exhaustion = 0.0f;
    client.hasHungerFeet = false;
    client.invulnerableUntil = now + 3.0;
    std::string why;
    Teleport(client, RespawnPoint(client, why));
    if (!why.empty()) SendChat(client, net::ChatMessage::Kind::Info, why);
    SendHealth(client, true);
}

void Server::DropOnDeath(ClientState& client) {
    if (!options.dropOnDeath || Creative(client)) return;
    // Half of each kind (whole units, rounded down: the other half's theirs to keep).
    ChestItems dropped{};
    bool any = false;
    for (size_t t = 0; t < dropped.size(); t++) {
        dropped[t] = client.resources[t] / 2;
        any |= dropped[t] > 0;
    }
    if (!any) return;
    // Into a chest in the nearest open cell where they died: their feet's cell, or one
    // around or just above it (air first; water or a wild plant will do, not a trap).
    const glm::ivec3 at(glm::floor(client.feet + glm::vec3(0.0f, 0.1f, 0.0f)));
    std::optional<glm::ivec3> spot;
    for (int pass = 0; pass < 2 && !spot; pass++)
        for (int dy = 0; dy <= 3 && !spot; dy++)
            for (int ring = 0; ring <= 1 && !spot; ring++)
                for (int dx = -ring; dx <= ring && !spot; dx++)
                    for (int dz = -ring; dz <= ring && !spot; dz++) {
                        if (std::max(std::abs(dx), std::abs(dz)) != ring) continue;
                        const glm::ivec3 p = at + glm::ivec3(dx, dy, dz);
                        Block b;
                        if (!world->GetBlock(p.x, p.y, p.z, b)) continue;
                        const bool ok = pass == 0 ? b.IsAir() : (b.IsLiquid() || (b.IsPlant() && b.type != BlockType::SPIKES));
                        if (ok) spot = p;
                    }
    if (!spot) return; // nowhere to put it: they keep it all
    world->SetBlock(spot->x, spot->y, spot->z, Block(BlockType::CHEST));
    chests[*spot] = dropped;
    for (size_t t = 0; t < dropped.size(); t++) client.resources[t] -= dropped[t];
    SendResources(client);
    char where[64];
    std::snprintf(where, sizeof where, "%d, %d, %d", spot->x, spot->y, spot->z);
    SendChat(client, net::ChatMessage::Kind::Info, std::string("Half of what you carried is in a chest where you died (") + where + ")");
}

void Server::HandleUseBed(ClientState& client, const glm::ivec3& pos) {
    Block block;
    if (!InReach(client, pos, kChestReach) || !world->GetBlock(pos.x, pos.y, pos.z, block)) return;
    if (block.type == BlockType::DOOR || block.type == BlockType::DOOR_OPEN) return ToggleDoor(pos, block.type == BlockType::DOOR);
    if (block.type == BlockType::TNT) return LightTnt(pos, kTntFuse);
    // A lever flips; a valve with no channel opens or shuts by hand.
    if (block.type == BlockType::LEVER || block.type == BlockType::LEVER_ON) {
        // A dungeon lever vault's levers, gates and floor gate get their channels the
        // first time one of its levers is flipped.
        if (!signalBlocks.count(pos))
            for (const auto& [p, config] : Dungeons::LeverVaultAt(terrain, pos).wiring)
                if (!signalBlocks.count(p)) {
                    signalBlocks[p].config = config;
                    BroadcastSignal(p);
                }
        Block flipped = block;
        flipped.type = block.type == BlockType::LEVER ? BlockType::LEVER_ON : BlockType::LEVER;
        world->SetBlock(pos.x, pos.y, pos.z, flipped);
        return;
    }
    // A filter pipe or a crafter: set to what's in hand.
    if (block.type == BlockType::FILTER_PIPE || block.type == BlockType::CRAFTER) {
        const BlockType held = client.heldBlock == BlockType::AIR ? BlockType::AIR : ResourceTypeOf(client.heldBlock);
        Block set = block;
        if (block.type == BlockType::FILTER_PIPE) {
            set.axis = static_cast<uint8_t>(held);
            SendChat(client, net::ChatMessage::Kind::Info, held == BlockType::AIR ? "The filter lets anything through" : std::string("The filter lets only ") + BlockId(held) + " through");
        }
        else {
            const auto& recipes = Crafting::Recipes();
            int found = -1;
            for (int i = 0; i < static_cast<int>(recipes.size()) && found < 0; i++)
                if (!recipes[i].MadeOnce() && recipes[i].output == held && held != BlockType::AIR) found = i;
            if (held == BlockType::AIR) {
                const int index = static_cast<int>(block.axis) - 1;
                return SendChat(client, net::ChatMessage::Kind::Info,
                                index >= 0 && index < static_cast<int>(recipes.size()) ? std::string("The crafter makes ") + recipes[index].name
                                                                                       : "The crafter has no recipe: right-click it with what it should make in hand");
            }
            if (found < 0) return SendChat(client, net::ChatMessage::Kind::Info, std::string("Nothing a crafter can make gives ") + BlockId(held));
            set.axis = static_cast<uint8_t>(found + 1);
            SendChat(client, net::ChatMessage::Kind::Info, std::string("The crafter makes ") + recipes[found].name);
        }
        world->SetBlock(pos.x, pos.y, pos.z, set);
        return;
    }
    if (block.type == BlockType::VALVE || block.type == BlockType::VALVE_OPEN) {
        auto sig = signalBlocks.find(pos);
        if (sig != signalBlocks.end() && sig->second.config.a != 0) {
            return SendChat(client, net::ChatMessage::Kind::Info, "This valve answers to channel " + std::to_string(sig->second.config.a));
        }
        world->SetBlock(pos.x, pos.y, pos.z, Block(block.type == BlockType::VALVE ? BlockType::VALVE_OPEN : BlockType::VALVE));
        return;
    }
    if (block.type == BlockType::CAMPFIRE) {
        // Cooks one of the raw food in their hand.
        const BlockType cooked = client.heldBlock == BlockType::RAW_VENISON ? BlockType::COOKED_VENISON
                               : client.heldBlock == BlockType::RAW_FISH    ? BlockType::COOKED_FISH
                                                                             : BlockType::AIR;
        uint32_t& raw = client.resources[static_cast<size_t>(client.heldBlock)];
        if (cooked == BlockType::AIR || raw < Block::kUnitsPerBlock) return;
        raw -= Block::kUnitsPerBlock;
        client.resources[static_cast<size_t>(cooked)] += Block::kUnitsPerBlock;
        SendResources(client);
        if (cooked == BlockType::COOKED_VENISON) Award(client, Achievement::Cook);
        return;
    }
    if (block.type != BlockType::BED) return;
    if (client.bed != pos) {
        client.bed = pos;
        SendChat(client, net::ChatMessage::Kind::Info, "You'll respawn at this bed");
    }
    // At night (the sun below the horizon), they sleep; once everyone here is asleep
    // (in bed within the last few seconds), it's morning.
    if (std::sin(timeOfDay * 6.283185307179586) >= 0.0) return SendChat(client, net::ChatMessage::Kind::Info, "You can only sleep at night");
    const double now = Now();
    client.sleptAt = now;
    int sleeping = 0, everyone = 0;
    for (const auto& [peer, other] : clients) {
        if (!other.welcomed) continue;
        everyone++;
        sleeping += now - other.sleptAt < kSleepWindow;
    }
    if (sleeping < everyone) {
        Broadcast(net::ChatMessage::Kind::Info, client.name + " is in bed (" + std::to_string(sleeping) + " of " + std::to_string(everyone) + " asleep)");
        return;
    }
    timeOfDay = 0.02; // just after sunrise
    for (auto& [peer, other] : clients) other.sleptAt = -1000.0;
    Broadcast(net::ChatMessage::Kind::Info, everyone > 1 ? "Everyone slept: it's morning" : "You slept: it's morning");
    for (auto& [peer, other] : clients)
        if (other.welcomed) Award(other, Achievement::SweetDreams);
    Log("night skipped by sleeping");
}

void Server::ToggleDoor(const glm::ivec3& pos, bool open) {
    // This half and the one above or below it.
    const BlockType from = open ? BlockType::DOOR : BlockType::DOOR_OPEN, to = open ? BlockType::DOOR_OPEN : BlockType::DOOR;
    std::vector<glm::ivec3> halves{ pos };
    for (const int dy : { 1, -1 }) {
        Block other;
        if (world->GetBlock(pos.x, pos.y + dy, pos.z, other) && other.type == from) {
            halves.push_back(pos + glm::ivec3(0, dy, 0));
            break;
        }
    }
    // It won't close on anyone standing in it.
    if (!open)
        for (const auto& [peer, other] : clients) {
            if (!other.welcomed || !other.hasState) continue;
            const Collision::AABB body = BodyAt(other.feet, 0.0f, (other.flags & net::kFlagCrouching) != 0);
            for (const auto& h : halves) {
                const glm::vec3 lo(h), hi = lo + glm::vec3(1.0f);
                if (glm::all(glm::lessThan(lo, body.max)) && glm::all(glm::greaterThan(hi, body.min))) return;
            }
        }
    for (const auto& h : halves) world->SetBlock(h.x, h.y, h.z, Block::Placed(to));
}

glm::vec3 Server::RespawnPoint(const ClientState& client, std::string& why) const {
    if (!client.bed) return SpawnPoint();
    const glm::ivec3 bed = *client.bed;
    Block b;
    if (!world->GetBlock(bed.x, bed.y, bed.z, b) || b.type != BlockType::BED) {
        why = "Your bed is gone, so you're back at the spawn point";
        return SpawnPoint();
    }
    auto open = [&](const glm::ivec3& p) { Block c; return world->GetBlock(p.x, p.y, p.z, c) && c.IsPassable(); };
    // On the bed, if there's headroom over it.
    if (open(bed + glm::ivec3(0, 1, 0)) && open(bed + glm::ivec3(0, 2, 0))) return glm::vec3(bed) + glm::vec3(0.5f, 0.55f, 0.5f);
    // Or standing beside it.
    for (const glm::ivec3 side : { glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1) }) {
        const glm::ivec3 p = bed + side;
        if (open(p) && open(p + glm::ivec3(0, 1, 0)) && !open(p - glm::ivec3(0, 1, 0))) return glm::vec3(p) + glm::vec3(0.5f, 0.0f, 0.5f);
    }
    why = "Your bed is blocked in, so you're back at the spawn point";
    return SpawnPoint();
}

void Server::UpdateBreath(ClientState& client, float dt) {
    using namespace Breathing;
    Air air;
    // In creative or no-clip, nothing takes your breath away.
    if (!Creative(client) && !(client.noclip && (client.flags & net::kFlagFlying))) {
        const glm::vec3 eye = client.feet + glm::vec3(0.0f, Breathing::kEyeHeight, 0.0f);
        const glm::ivec3 cell(glm::floor(eye));
        Block block;
        const bool underWater = world->GetBlock(cell.x, cell.y, cell.z, block) && UnderWater(block, eye.y - cell.y);
        air = At(eye.y, underWater);
    }
    client.airCause = air.cause;
    const float before = client.air;
    client.air = Step(client.air, air, dt);
    if (Suffocating(client.air, air)) {
        const double now = Now();
        if (before > 0.0f) client.nextBreathHit = now; // the first hit comes straight away
        if (now >= client.nextBreathHit) {
            const bool drowning = air.cause == Cause::Water;
            client.nextBreathHit = now + (drowning ? kDrownInterval : kSuffocateInterval);
            Hurt(client, drowning ? kDrownDamage : kSuffocateDamage,
                 drowning                       ? "drowned"
                 : air.cause == Cause::ThinAir ? "ran out of breath in the thin air up high"
                                                : "suffocated in the stale air deep underground",
                 false);
        }
    }
    if (static_cast<int>(std::ceil(client.air)) != client.sentAir || client.airCause != client.sentAirCause) SendHealth(client, false);
}

void Server::UpdateHunger(ClientState& client, float dt) {
    const bool flying = client.noclip && (client.flags & net::kFlagFlying);
    if (Creative(client) || flying) {
        client.hasHungerFeet = false;
        return;
    }
    // Time, and walking (not teleports or respawns: those jump far in one tick).
    client.exhaustion += dt / Hunger::kSecondsPerPoint * options.hungerRate;
    if (client.hasHungerFeet) {
        const float walked = glm::length(glm::vec2(client.feet.x - client.hungerFeet.x, client.feet.z - client.hungerFeet.z));
        if (walked < 5.0f) client.exhaustion += walked / Hunger::kBlocksPerPoint * options.hungerRate;
    }
    client.hungerFeet = client.feet;
    client.hasHungerFeet = true;
    client.food = Hunger::Drain(client.food, client.exhaustion);

    // Starving: hurts now and then, down to the last heart.
    const double now = Now();
    if (client.food == 0 && now >= client.nextStarve) {
        client.nextStarve = now + Hunger::kStarveInterval / std::max(1.0f, options.hungerRate);
        if (client.health > Hunger::kStarveFloor) Hurt(client, 1, "starved", false);
    }
    if (client.food != client.sentFood) SendHealth(client, false);
}

void Server::HandleEat(ClientState& client, BlockType food) {
    const int value = Hunger::FoodValue(food);
    uint32_t& held = client.resources[static_cast<size_t>(food)];
    // Food you hold, while not full (creative players have it all and aren't hungry).
    if (value == 0 || Creative(client) || held < Block::kUnitsPerBlock || client.food >= Hunger::kMaxFood) return SendResources(client);
    held -= Block::kUnitsPerBlock;
    client.food = std::min(Hunger::kMaxFood, client.food + value);
    Award(client, Achievement::FirstMeal);
    SendResources(client);
    SendHealth(client, false);
}

void Server::HandleOpenChest(ClientState& client, const glm::ivec3& pos, bool open) {
    client.openChest.reset();
    Block block;
    if (open && InReach(client, pos, kChestReach) && world->GetBlock(pos.x, pos.y, pos.z, block) && block.type == BlockType::CHEST) {
        client.openChest = pos;
        const bool unopened = chests.count(pos) == 0;
        const ChestItems& chest = ChestAt(pos);
        // The first look into a dungeon's chest.
        if (unopened && std::any_of(chest.begin(), chest.end(), [](uint32_t u) { return u > 0; })) Award(client, Achievement::TreasureHunter);
    }
    SendChest(client);
}

void Server::HandleChestMove(ClientState& client, const glm::ivec3& pos, BlockType type, uint32_t units, bool intoChest) {
    // Only with that chest open, still there and in reach.
    Block block;
    if (client.openChest != pos || !InReach(client, pos, kChestReach) || !world->GetBlock(pos.x, pos.y, pos.z, block) || block.type != BlockType::CHEST ||
        type == BlockType::AIR || block.IsLiquid())
        return SendResources(client);
    ChestItems& chest = chests[pos];
    uint32_t& from = intoChest ? client.resources[static_cast<size_t>(type)] : chest[static_cast<size_t>(type)];
    uint32_t& to = intoChest ? chest[static_cast<size_t>(type)] : client.resources[static_cast<size_t>(type)];
    units = std::min(units, from);
    from -= units;
    to += units;
    SendResources(client);
    SendChestToViewers(pos);
}

void Server::SendChest(ClientState& client) {
    net::ChestContents m;
    m.open = client.openChest.has_value();
    if (client.openChest) {
        m.pos = *client.openChest;
        const ChestItems& chest = chests[*client.openChest];
        for (size_t t = 0; t < chest.size(); t++)
            if (chest[t] > 0) m.held.emplace_back(static_cast<BlockType>(t), chest[t]);
    }
    net.Send(client.peer, net::kChannelReliable, net::Encode(m));
}

void Server::SendChestToViewers(const glm::ivec3& pos) {
    for (auto& [peer, client] : clients)
        if (client.openChest == pos) SendChest(client);
}

Server::ChestItems& Server::ChestAt(const glm::ivec3& pos) {
    auto it = chests.find(pos);
    if (it != chests.end()) return it->second;
    // Never opened: placed chests are always recorded, so this is a dungeon's, and its
    // loot is rolled now (from its position, so it's the same whenever it's found).
    ChestItems& chest = chests[pos];
    chest.fill(0);
    for (const auto& [type, units] : Dungeons::LootFor(terrain, pos)) chest[static_cast<size_t>(type)] += units;
    return chest;
}

void Server::RemoveChest(const glm::ivec3& pos, ClientState* breaker) {
    if (breaker) ChestAt(pos); // (an unopened dungeon chest still holds its loot)
    auto it = chests.find(pos);
    if (it == chests.end()) return;
    // What was in it goes to whoever broke it.
    if (breaker) {
        for (size_t t = 0; t < it->second.size(); t++) breaker->resources[t] += it->second[t];
        SendResources(*breaker);
    }
    chests.erase(it);
    for (auto& [peer, client] : clients)
        if (client.openChest == pos) {
            client.openChest.reset();
            SendChest(client);
        }
}

void Server::HandleShoot(ClientState& client, const glm::vec3& direction) {
    const double now = Now();
    // A bow of their own, an arrow to shoot (survival), and not too often.
    if (now < client.nextShot || Usable(client, Tool::Bow) != Tool::Bow) return;
    uint32_t& held = client.resources[static_cast<size_t>(BlockType::ARROW)];
    if (!Creative(client)) {
        if (held < Block::kUnitsPerBlock) return SendResources(client);
        held -= Block::kUnitsPerBlock;
        SendResources(client);
    }
    client.nextShot = now + kShotInterval * 0.8; // (a little slack for timing)
    const glm::vec3 dir = glm::normalize(direction);
    const glm::vec3 eye = client.feet + glm::vec3(0.0f, (client.flags & net::kFlagCrouching) ? 1.27f : kEyeHeight, 0.0f);
    arrows.push_back({ eye + dir * 0.4f, dir * kArrowSpeed, client.id, now });
    if (arrows.size() > 256) arrows.erase(arrows.begin()); // (the oldest go first)
}

void Server::LightTnt(const glm::ivec3& pos, double fuse) {
    if (fuses.count(pos)) return;
    world->SetBlock(pos.x, pos.y, pos.z, Block(BlockType::TNT_LIT));
    fuses[pos] = Now() + fuse;
}

void Server::UpdateTnt() {
    const double now = Now();
    std::vector<glm::ivec3> due;
    for (auto it = fuses.begin(); it != fuses.end();) {
        Block b;
        if (world->GetBlock(it->first.x, it->first.y, it->first.z, b) && b.type != BlockType::TNT_LIT) { it = fuses.erase(it); continue; } // (broken first)
        if (now >= it->second) {
            due.push_back(it->first);
            it = fuses.erase(it);
        }
        else ++it;
    }
    for (const auto& p : due) Explode(p, kTntRadius);
}

void Server::Explode(const glm::ivec3& pos, int radius) {
    // Everything in a ball goes (water and dungeon bricks too; a chest's contents are
    // lost), and TNT caught in it is set off on a short fuse.
    for (int x = -radius; x <= radius; x++)
        for (int y = -radius; y <= radius; y++)
            for (int z = -radius; z <= radius; z++) {
                if (x * x + y * y + z * z > radius * radius + 1) continue;
                const glm::ivec3 p = pos + glm::ivec3(x, y, z);
                Block b;
                if (!world->GetBlock(p.x, p.y, p.z, b) || b.IsAir()) continue;
                if (b.type == BlockType::TNT && p != pos) {
                    LightTnt(p, 0.4 + 0.1 * (std::abs(x) + std::abs(z)));
                    continue;
                }
                if (b.type == BlockType::CHEST) RemoveChest(p, nullptr);
                if (b.type == BlockType::SIGN && signs.erase(p)) BroadcastSign(p);
                world->SetBlock(p.x, p.y, p.z, Block());
            }
    const auto packet = net::Encode(net::Explosion{ pos, static_cast<uint8_t>(radius) });
    const glm::vec3 centre = glm::vec3(pos) + glm::vec3(0.5f);
    for (auto& [peer, client] : clients) {
        if (!client.welcomed) continue;
        net.Send(peer, net::kChannelReliable, packet);
        // Hurt (more the closer), and thrown away from it.
        const float d = glm::distance(client.feet + glm::vec3(0.0f, 0.9f, 0.0f), centre);
        if (d > radius * 2.0f) continue;
        const int damage = static_cast<int>(std::lround(10.0f * (1.0f - d / (radius * 2.0f))));
        if (damage > 0) Hurt(client, damage, "was blown up");
        glm::vec3 away = client.feet - centre;
        away.y = 0.0f;
        away = glm::length(away) > 1e-3f ? glm::normalize(away) : glm::vec3(1.0f, 0.0f, 0.0f);
        net.Send(peer, net::kChannelReliable, net::Encode(net::Knockback{ away * 8.0f * (1.0f - d / (radius * 2.0f)) + glm::vec3(0.0f, 6.0f, 0.0f) }));
    }
    // Creatures too (ids first: a killed one leaves the list).
    std::vector<std::pair<uint16_t, int>> struck;
    for (const auto& c : creatures.All()) {
        const float d = glm::distance(c.feet, centre);
        if (d <= radius * 2.0f) struck.emplace_back(c.id, static_cast<int>(std::lround(12.0f * (1.0f - d / (radius * 2.0f)))) + 1);
    }
    for (const auto& [id, damage] : struck) {
        bool killed = false;
        creatures.Damage(id, damage, centre, Now(), killed);
    }
}

void Server::HandleWriteSign(ClientState& client, const glm::ivec3& pos, const std::string& raw) {
    Block block;
    if (!InReach(client, pos, kChestReach) || !world->GetBlock(pos.x, pos.y, pos.z, block) || block.type != BlockType::SIGN) return;
    // Printable characters only.
    std::string text;
    for (char c : raw)
        if (c >= 32 && c < 127) text += c;
    signs[pos] = text.substr(0, net::kMaxSignLength);
    BroadcastSign(pos);
}

void Server::BroadcastSign(const glm::ivec3& pos) {
    auto it = signs.find(pos);
    const auto packet = net::Encode(net::SignText{ pos, it != signs.end() ? it->second : std::string() }, false);
    for (auto& [peer, client] : clients)
        if (client.welcomed) net.Send(peer, net::kChannelReliable, packet);
}

void Server::HandleCast(ClientState& client, const glm::ivec3& pos, bool cast) {
    client.bobber.reset();
    Block water;
    // Into water (with nothing on top of it) within 12 blocks, with a rod of their own.
    if (cast && Usable(client, Tool::Rod) == Tool::Rod && InReach(client, pos, 12.5f) && world->GetBlock(pos.x, pos.y, pos.z, water) && water.HasWater()) {
        Block above;
        if (!world->GetBlock(pos.x, pos.y + 1, pos.z, above) || !above.HasWater()) {
            client.bobber = pos;
            client.castFrom = client.feet;
            weatherSeed = weatherSeed * 1664525u + 1013904223u;
            client.biteAt = Now() + (4.0 + 10.0 * ((weatherSeed >> 8) / double(1u << 24))) / std::max(0.1f, options.fishingRate);
        }
    }
    net.Send(client.peer, net::kChannelReliable, net::Encode(net::Bobber{ pos, static_cast<uint8_t>(client.bobber ? 1 : 0), 0 }));
}

void Server::UpdateFishing() {
    const double now = Now();
    for (auto& [peer, client] : clients) {
        if (!client.bobber) continue;
        const glm::ivec3 at = *client.bobber;
        Block water;
        // Walked off (or the water's gone): the line's reeled in.
        if (glm::distance(client.feet, client.castFrom) > 3.0f || !world->GetBlock(at.x, at.y, at.z, water) || !water.HasWater()) {
            client.bobber.reset();
            net.Send(peer, net::kChannelReliable, net::Encode(net::Bobber{ at, 0, 0 }));
            continue;
        }
        if (now < client.biteAt) continue;
        // A bite: the fish is theirs.
        client.bobber.reset();
        client.resources[static_cast<size_t>(BlockType::RAW_FISH)] += Block::kUnitsPerBlock;
        SendResources(client);
        SendChat(client, net::ChatMessage::Kind::Info, "You caught a fish!");
        net.Send(peer, net::kChannelReliable, net::Encode(net::Bobber{ at, 0, 1 }));
        Award(client, Achievement::Angler);
    }
}

void Server::UpdateArrows() {
    const double now = Now();
    constexpr float dt = 1.0f / kTickRate;
    constexpr int kSteps = 6;
    for (auto it = arrows.begin(); it != arrows.end();) {
        Arrow& a = *it;
        // Stuck arrows stay a while; flying ones don't fly forever.
        if ((a.stuckAt >= 0.0 && now - a.stuckAt > 6.0) || (a.stuckAt < 0.0 && now - a.born > 8.0)) {
            it = arrows.erase(it);
            continue;
        }
        if (a.stuckAt >= 0.0) { ++it; continue; }
        bool gone = false;
        for (int step = 0; step < kSteps && !gone && a.stuckAt < 0.0; step++) {
            a.velocity.y -= kArrowGravity * dt / kSteps;
            const glm::vec3 next = a.position + a.velocity * (dt / kSteps);
            // Into a block: stuck there.
            const glm::ivec3 cell(glm::floor(next));
            Block b;
            if (world->GetBlock(cell.x, cell.y, cell.z, b) && !b.IsPassable() &&
                (b.IsFullBlock() || next.y - cell.y <= b.SurfaceHeight(next.x - cell.x, next.z - cell.z))) {
                a.stuckAt = now;
                a.velocity = glm::vec3(0.0f);
                break;
            }
            a.position = next;
            auto inside = [&](const Collision::AABB& box) {
                return glm::all(glm::greaterThanEqual(a.position, box.min - 0.1f)) && glm::all(glm::lessThanEqual(a.position, box.max + 0.1f));
            };
            // A creature.
            for (const auto& c : creatures.All()) {
                if (!inside(CreatureSystem::Box(c))) continue;
                bool killed = false;
                const uint16_t id = c.id;
                const CreatureType type = c.type;
                creatures.Damage(id, kArrowDamage, a.position - a.velocity * 0.05f, now, killed);
                if (!killed && a.shooter != kCreatureShooter) creatures.Provoke(id, a.shooter, now);
                if (killed)
                    for (auto& [peer, shooter] : clients)
                        if (shooter.id == a.shooter) {
                            SendChat(shooter, net::ChatMessage::Kind::Info, std::string("You shot a ") + SpecOf(type).id);
                            Award(shooter, Achievement::Marksman);
                            if (type != CreatureType::Skeleton) Award(shooter, type == CreatureType::Deer ? Achievement::Hunter : Achievement::GhoulSlayer);
                        }
                gone = true;
                break;
            }
            if (gone) break;
            // A player (not the shooter as it leaves the bow; not when PvP is off).
            for (auto& [peer, target] : clients) {
                if (!target.welcomed || !target.hasState || (target.noclip && (target.flags & net::kFlagFlying))) continue;
                if (target.id == a.shooter && now - a.born < 0.3) continue;
                if (!inside(BodyAt(target.feet, 0.0f, (target.flags & net::kFlagCrouching) != 0))) continue;
                if (options.pvp || target.id == a.shooter || a.shooter == kCreatureShooter) {
                    std::string by = a.shooter == kCreatureShooter ? "a skeleton" : "someone";
                    for (const auto& [p2, s] : clients)
                        if (s.id == a.shooter) by = s.name;
                    const double before = target.invulnerableUntil;
                    Hurt(target, kArrowDamage, "was shot by " + by);
                    if (target.health > 0 && target.lastHurt >= now && target.invulnerableUntil == before) {
                        const glm::vec2 push = glm::length(glm::vec2(a.velocity.x, a.velocity.z)) > 1e-3f ? glm::normalize(glm::vec2(a.velocity.x, a.velocity.z)) : glm::vec2(0.0f);
                        net.Send(target.peer, net::kChannelReliable, net::Encode(net::Knockback{ glm::vec3(push.x * 5.0f, 4.0f, push.y * 5.0f) }));
                    }
                }
                gone = true;
                break;
            }
        }
        if (gone) it = arrows.erase(it);
        else ++it;
    }
}

void Server::SendArrows() {
    constexpr float kSendRange = 96.0f;
    for (auto& [peer, client] : clients) {
        if (!client.welcomed) continue;
        net::Arrows m;
        for (const auto& a : arrows)
            if (glm::distance(a.position, client.feet) < kSendRange) m.arrows.push_back({ a.position, a.velocity });
        if (m.arrows.empty() && !client.sentArrows) continue; // (nothing, and they know it)
        client.sentArrows = !m.arrows.empty();
        net.Send(peer, net::kChannelUnreliable, net::Encode(m));
    }
}

void Server::Award(ClientState& client, Achievement a) {
    if (Creative(client) || (client.achievements & Achievements::Bit(a))) return;
    client.achievements |= Achievements::Bit(a);
    SendChat(client, net::ChatMessage::Kind::Info, std::string(Achievements::kToastMark) + Achievements::Name(a) + ": " + Achievements::Description(a));
    for (auto& [peer, other] : clients)
        if (other.welcomed && other.id != client.id) SendChat(other, net::ChatMessage::Kind::Info, client.name + " earned " + Achievements::Name(a));
}

bool Server::GrowTree(const glm::ivec3& at, bool pine) {
    const int height = 4 + static_cast<int>(Noise::Random01(at.x, at.y, at.z, 0x7EE5u) * 3.0f); // 4..6
    auto open = [&](const glm::ivec3& p) { Block b; return world->GetBlock(p.x, p.y, p.z, b) && (b.IsAir() || b.IsPlant()); };
    for (int y = 1; y <= height + 1; y++)
        if (!open(at + glm::ivec3(0, y, 0))) return false; // no room
    // Leaves first (into open cells), then the trunk through them.
    const Block leaves(pine ? BlockType::PINE_LEAVES : BlockType::LEAVES);
    if (pine) {
        // A cone: wide near the bottom of the crown, narrowing to a tip.
        for (int y = 2; y <= height + 1; y++) {
            const int r = std::max(0, (height + 1 - y) / 2 + (y < height ? 1 : 0));
            for (int x = -r; x <= r; x++)
                for (int z = -r; z <= r; z++)
                    if (std::abs(x) + std::abs(z) <= r + 1 && open(at + glm::ivec3(x, y, z))) world->SetBlock(at.x + x, at.y + y, at.z + z, leaves);
        }
    }
    else {
        // A round crown around the top of the trunk.
        const glm::ivec3 top = at + glm::ivec3(0, height, 0);
        for (int x = -2; x <= 2; x++)
            for (int y = -2; y <= 1; y++)
                for (int z = -2; z <= 2; z++) {
                    if (x * x + y * y * 2 + z * z > 6) continue;
                    const glm::ivec3 p = top + glm::ivec3(x, y, z);
                    if (open(p)) world->SetBlock(p.x, p.y, p.z, leaves);
                }
    }
    for (int y = 0; y < height; y++) world->SetBlock(at.x, at.y + y, at.z, Block(BlockType::LOG));
    return true;
}

void Server::UpdateCrops() {
    // Once a second, each crop (in a loaded chunk) may ripen: on average after 90 s.
    const double now = Now();
    if (now < nextCropUpdate) return;
    nextCropUpdate = now + 1.0;
    const float chance = std::min(1.0f, options.cropGrowthRate / 90.0f);
    for (auto it = crops.begin(); it != crops.end();) {
        Block b;
        const glm::ivec3 p = *it;
        if (!world->GetBlock(p.x, p.y, p.z, b)) { ++it; continue; } // not loaded: waits
        const bool sapling = b.type == BlockType::SAPLING || b.type == BlockType::PINE_SAPLING;
        if (b.type != BlockType::CROP && !sapling) { it = crops.erase(it); continue; } // trampled, washed away
        if (sapling) {
            // Saplings take longer (two minutes on average), and need room to grow.
            if (Noise::Random01(p.x, p.y, p.z, static_cast<uint32_t>(tickCount) * 2246822519u) < chance * 0.75f && GrowTree(p, b.type == BlockType::PINE_SAPLING)) {
                it = crops.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        if (Noise::Random01(p.x, p.y, p.z, static_cast<uint32_t>(tickCount) * 2654435761u) < chance) {
            world->SetBlock(p.x, p.y, p.z, Block::Plant(BlockType::RIPE_WHEAT, b.edgeData.edges[0]));
            it = crops.erase(it);
            continue;
        }
        ++it;
    }
}

void Server::UpdateTraps() {
    const double now = Now();
    // Breath goes by real time, not ticks: on a slow, overloaded machine ticks run late,
    // and air shouldn't last longer for it.
    const float breathStep = lastTrapsTime > 0.0 ? static_cast<float>(std::clamp(now - lastTrapsTime, 0.0, 0.5)) : 1.0f / kTickRate;
    lastTrapsTime = now;
    for (auto& [peer, client] : clients) {
        if (!client.welcomed || !client.hasState) continue;
        UpdateHunger(client, breathStep);
        // Walked away from their open chest: it closes.
        if (client.openChest && !InReach(client, *client.openChest, kChestReach + 1.0f)) {
            client.openChest.reset();
            SendChest(client);
        }
        // Slow healing once nothing has hurt them for a while, while well fed (in
        // survival; it costs food).
        const bool survival = !Creative(client);
        if (client.health < kMaxHealth && now >= client.nextRegen && (!survival || client.food > Hunger::kRegenFood)) {
            const int healed = std::min(kMaxHealth, client.health + 2) - client.health;
            client.health += healed;
            client.nextRegen = now + kRegenInterval;
            if (survival) {
                client.exhaustion += healed * Hunger::kPointsPerHealing;
                client.food = Hunger::Drain(client.food, client.exhaustion);
            }
            SendHealth(client, false);
        }
        UpdateBreath(client, breathStep);
        if (client.noclip && (client.flags & net::kFlagFlying)) continue; // flying through it all

        // The cells the body's feet are in, and the cells just under them, at the
        // corners of its footprint.
        const float half = 0.29f;
        glm::ivec3 feetCells[4], underCells[4];
        int n = 0;
        for (const float dx : { -half, half })
            for (const float dz : { -half, half }) {
                feetCells[n] = glm::ivec3(glm::floor(client.feet + glm::vec3(dx, 0.01f, dz)));
                underCells[n] = glm::ivec3(glm::floor(client.feet + glm::vec3(dx, -0.05f, dz)));
                n++;
            }
        bool spiked = false, burning = false;
        for (int i = 0; i < 4; i++) {
            Block b;
            const glm::ivec3& f = feetCells[i];
            if (world->GetBlock(f.x, f.y, f.z, b)) {
                if (b.type == BlockType::SPIKES) spiked = true;
                if (b.type == BlockType::CAMPFIRE) burning = true;
                if (b.type == BlockType::PRESSURE_PLATE) {
                    const bool fresh = !plateSignal.count(f);
                    PressPlate(f, now);
                    if (fresh && signalBlocks.count(f) && signalBlocks[f].config.out) SendChat(client, net::ChatMessage::Kind::Info, "*click*");
                }
            }
            const glm::ivec3& u = underCells[i];
            if (world->GetBlock(u.x, u.y, u.z, b) && b.type == BlockType::CRUMBLING_BRICK && !crumbling.count(u)) crumbling[u] = now + kCrumbleDelay;
        }
        if (spiked && now >= client.nextSpikeHit) {
            client.nextSpikeHit = now + kSpikeInterval;
            Hurt(client, kSpikeDamage, "was impaled on spikes");
        }
        if (burning && now >= client.nextSpikeHit) {
            client.nextSpikeHit = now + 0.75;
            Hurt(client, 1, "walked into a campfire");
        }
    }

    // Crumbling bricks whose time has come fall away, and set off those touching them.
    std::vector<glm::ivec3> due;
    for (const auto& [pos, when] : crumbling)
        if (now >= when) due.push_back(pos);
    for (const auto& pos : due) {
        crumbling.erase(pos);
        Block b;
        if (!world->GetBlock(pos.x, pos.y, pos.z, b) || b.type != BlockType::CRUMBLING_BRICK) continue;
        world->SetBlock(pos.x, pos.y, pos.z, Block());
        for (const glm::ivec3 d : { glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1) }) {
            const glm::ivec3 q = pos + d;
            if (world->GetBlock(q.x, q.y, q.z, b) && b.type == BlockType::CRUMBLING_BRICK && !crumbling.count(q)) crumbling[q] = now + kCrumbleSpread;
        }
    }
    // Creatures set traps off too (not bats, which fly): plates, spikes, and crumbling
    // bricks under them.
    std::vector<std::pair<uint16_t, glm::vec3>> spiked;
    for (const auto& c : creatures.All()) {
        if (c.type == CreatureType::Bat) continue;
        const glm::ivec3 f(glm::floor(c.feet + glm::vec3(0.0f, 0.01f, 0.0f))), u(glm::floor(c.feet - glm::vec3(0.0f, 0.05f, 0.0f)));
        Block b;
        if (world->GetBlock(f.x, f.y, f.z, b)) {
            if (b.type == BlockType::PRESSURE_PLATE) PressPlate(f, now);
            double& next = creatureSpikeAt[c.id];
            if (b.type == BlockType::SPIKES && now >= next) {
                next = now + kSpikeInterval;
                spiked.emplace_back(c.id, c.feet);
            }
        }
        if (world->GetBlock(u.x, u.y, u.z, b) && b.type == BlockType::CRUMBLING_BRICK && !crumbling.count(u)) crumbling[u] = now + kCrumbleDelay;
    }
    for (const auto& [id, feet] : spiked) {
        bool killed = false;
        creatures.Damage(id, kSpikeDamage, feet - glm::vec3(0.0f, 1.0f, 0.0f), now, killed);
    }
    if (creatureSpikeAt.size() > 4 * CreatureSystem::kMaxCreatures) creatureSpikeAt.clear();
}

void Server::PressPlate(const glm::ivec3& f, double now) {
    // Sending on its channel, if it has one. A dungeon flood chamber's plate and
    // sluice gates get theirs the first time it's pressed.
    if (!plateSignal.count(f) && !signalBlocks.count(f))
        for (const auto& [p, config] : Dungeons::FloodChamberSignals(terrain, f))
            if (!signalBlocks.count(p)) {
                signalBlocks[p].config = config;
                BroadcastSignal(p);
            }
    plateSignal[f] = now + Signals::kPlateSeconds;
}

void Server::UpdateHorde() {
    // Night as players see it: the sun well down.
    const float sun = static_cast<float>(std::sin(timeOfDay * 6.283185307179586));
    const bool night = sun < -0.12f;
    if (night == wasNight) return;
    wasNight = night;
    if (night && creatures.NaturalSpawning()) {
        weatherSeed = weatherSeed * 1664525u + 1013904223u;
        if ((weatherSeed >> 8) % kHordeOdds == 0) SetHorde(true);
    }
    else if (!night && creatures.Horde()) SetHorde(false);
}

void Server::SetHorde(bool on) {
    if (on == creatures.Horde()) return;
    creatures.SetHorde(on);
    Broadcast(net::ChatMessage::Kind::Info, on ? "The ground stirs... a horde is coming tonight." : "The horde fades with the dawn.");
    Log(on ? "a horde night begins" : "the horde night ends");
}

// --- Tick -------------------------------------------------------------------------

void Server::Tick() {
    tickCount++;
    // VOXEL_BENCH: where tick time goes, printed once a second (see Bench below).
    static const bool bench = std::getenv("VOXEL_BENCH") != nullptr;
    double mark = bench ? Now() : 0.0;
    const double tickStart = mark;
    auto section = [&](int i) {
        if (!bench) return;
        const double t = Now();
        benchSections[i] += t - mark;
        mark = t;
    };
    RunConsoleCommands();
    UpdateSky();
    UpdateHorde();
    UpdateTraps();
    section(0);
    UpdateCreatures();
    section(1);
    UpdateCrops();
    UpdateArrows();
    UpdateFishing();
    UpdateTnt();
    section(2);
    UpdatePower();
    section(3);
    UpdateSignals();
    section(4);
    if (storage && options.autosaveSeconds > 0.0f && Now() - lastSaveTime >= options.autosaveSeconds) Save(false);

    std::vector<glm::ivec3> centers;
    for (auto& [peer, client] : clients)
        if (client.welcomed) centers.push_back(Coords::ChunkOf(glm::ivec3(glm::floor(client.feet))));
    std::sort(centers.begin(), centers.end(), [](const glm::ivec3& a, const glm::ivec3& b) {
        return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
    });
    centers.erase(std::unique(centers.begin(), centers.end()), centers.end());
    world->Update(centers);

    DistributeChanges();
    for (auto& [peer, client] : clients)
        if (client.welcomed) StreamWorld(client);
    BroadcastPlayers();
    SendCreatures();
    SendArrows();
    net.Flush();
    section(5);
    if (bench) {
        const double took = Now() - tickStart;
        benchTicks++;
        benchWorst = std::max(benchWorst, took);
        if (tickStart - benchLast >= 1.0) {
            const double n = static_cast<double>(benchTicks);
            const auto water = world->GetWater().TakeBench();
            std::printf("[server] %d ticks: %.2f ms avg, %.2f worst | traps %.2f creatures %.2f misc %.2f power %.2f signals %.2f world+send %.2f ms | "
                        "%zu machines, %zu air networks, %zu signal blocks | water: %d ticks, %.2f ms avg, %.2f worst, %zu active, %zu pumps+springs\n",
                        benchTicks,
                        (benchSections[0] + benchSections[1] + benchSections[2] + benchSections[3] + benchSections[4] + benchSections[5]) / n * 1e3, benchWorst * 1e3, benchSections[0] / n * 1e3, benchSections[1] / n * 1e3, benchSections[2] / n * 1e3,
                        benchSections[3] / n * 1e3, benchSections[4] / n * 1e3, benchSections[5] / n * 1e3, machineList.size(), networks.size(), signalBlocks.size(),
                        water.ticks, water.ticks ? water.seconds / water.ticks * 1e3 : 0.0, water.worst * 1e3, world->GetWater().GetActiveCount(),
                        world->GetWater().GetPumpCount());
            std::fflush(stdout);
            benchLast = tickStart;
            benchTicks = 0;
            benchWorst = 0.0;
            for (double& s : benchSections) s = 0.0;
        }
    }
}

void Server::DistributeChanges() {
    world->TakeChanges(changedBlocks, changedLight);
    if (clients.empty()) return;
    for (const auto& pos : changedBlocks) {
        const glm::ivec3 chunk = Coords::ChunkOf(pos);
        const uint16_t index = LocalIndex(pos);
        for (auto& [peer, client] : clients)
            if (client.sent.count(chunk)) client.pendingCells[chunk].insert(index);
    }
    for (const auto& chunk : changedLight)
        for (auto& [peer, client] : clients)
            if (client.sent.count(chunk)) client.pendingLight.insert(chunk);
}

size_t Server::SendChunk(ClientState& client, const glm::ivec3& coords) {
    auto chunk = world->FindChunk(coords);
    // Read the version first: if light is published meanwhile, it's simply sent again.
    const uint32_t version = chunk->GetLightVersion();
    chunk->CopyBlocks(*blockScratch);
    chunk->CopyPublishedLight(*lightScratch);
    const auto packet = net::EncodeChunkData(coords, *blockScratch, *lightScratch);
    net.Send(client.peer, net::kChannelReliable, packet);
    client.sent[coords] = version;
    client.pendingCells.erase(coords);
    client.pendingLight.erase(coords);
    return packet.size();
}

void Server::StreamWorld(ClientState& client) {
    if (net.GetStats(client.peer).queuedReliableBytes > kMaxQueuedBytes) return; // let the link catch up

    const glm::ivec3 center = Coords::ChunkOf(glm::ivec3(glm::floor(client.feet)));
    const int r = client.viewDistance, v = client.verticalViewDistance;
    auto unload = [&](const glm::ivec3& coords) {
        net.Send(client.peer, net::kChannelReliable, net::Encode(net::ChunkUnload{ coords }));
        client.pendingCells.erase(coords);
        client.pendingLight.erase(coords);
    };

    // Moved to another chunk: drop what's now out of range (with the same one chunk of
    // hysteresis the server's own unloading uses), and look for new chunks again.
    if (!client.hasCenter || center != client.center) {
        client.center = center;
        client.hasCenter = true;
        client.allSent = false;
        for (auto it = client.sent.begin(); it != client.sent.end();) {
            const glm::ivec3 offset = it->first - center;
            const glm::ivec3 d = glm::abs(offset);
            if (d.x > r + 1 || d.z > r + 1 || offset.y < -v - 1 || offset.y >= v + 1) {
                unload(it->first);
                it = client.sent.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    size_t budget = kBytesPerTick;
    auto spend = [&](size_t bytes) { budget = bytes >= budget ? 0 : budget - bytes; };
    auto byDistance = [&](auto& keys) {
        std::sort(keys.begin(), keys.end(),
                  [&](const glm::ivec3& a, const glm::ivec3& b) { return DistanceSquared(a, center) < DistanceSquared(b, center); });
    };

    // 1. Changed blocks, nearest chunks first.
    if (!client.pendingCells.empty()) {
        std::vector<glm::ivec3> order;
        order.reserve(client.pendingCells.size());
        for (const auto& [coords, cells] : client.pendingCells) order.push_back(coords);
        byDistance(order);
        for (const auto& coords : order) {
            if (budget == 0) break;
            auto chunk = world->FindChunk(coords);
            if (!chunk || !chunk->IsLoaded()) {
                unload(coords);
                client.sent.erase(coords);
                continue;
            }
            const auto& cells = client.pendingCells[coords];
            if (cells.size() > kResendThreshold) {
                spend(SendChunk(client, coords));
                continue;
            }
            net::BlockDeltas deltas;
            deltas.coords = coords;
            deltas.cells.reserve(cells.size());
            for (uint16_t index : cells) {
                const glm::ivec3 l = LocalFromIndex(index);
                deltas.cells.push_back({ index, chunk->GetBlock(l.x, l.y, l.z) });
            }
            const auto packet = net::Encode(deltas);
            net.Send(client.peer, net::kChannelReliable, packet);
            spend(packet.size());
            client.pendingCells.erase(coords);
        }
    }

    // 2. Changed light.
    if (!client.pendingLight.empty() && budget > 0) {
        std::vector<glm::ivec3> order(client.pendingLight.begin(), client.pendingLight.end());
        byDistance(order);
        for (const auto& coords : order) {
            if (budget == 0) break;
            client.pendingLight.erase(coords);
            auto chunk = world->FindChunk(coords);
            auto sentIt = client.sent.find(coords);
            if (!chunk || sentIt == client.sent.end()) continue;
            const uint32_t version = chunk->GetLightVersion();
            if (version == sentIt->second) continue;
            chunk->CopyPublishedLight(*lightScratch);
            const auto packet = net::EncodeLightData(coords, *lightScratch);
            net.Send(client.peer, net::kChannelReliable, packet);
            sentIt->second = version;
            spend(packet.size());
        }
    }

    // 3. Chunks the client doesn't have yet, nearest first, once generated and lit.
    if (!client.allSent && budget > 0) {
        bool missing = false;
        for (const auto& offset : loadOffsets) {
            // (Only as far as this player sees.)
            if (std::abs(offset.x) > r || std::abs(offset.z) > r || offset.y < -v || offset.y >= v) continue;
            const glm::ivec3 coords = center + offset;
            if (client.sent.count(coords)) continue;
            auto chunk = world->FindChunk(coords);
            if (!chunk || !chunk->IsLoaded() || chunk->GetLightVersion() == 0) {
                missing = true;
                continue;
            }
            if (budget == 0) {
                missing = true;
                break;
            }
            spend(SendChunk(client, coords));
        }
        client.allSent = !missing;
    }
}

void Server::BroadcastPlayers() {
    net::PlayerSnapshot snapshot;
    snapshot.tick = tickCount;
    snapshot.timeOfDay = static_cast<float>(timeOfDay);
    snapshot.rain = static_cast<uint8_t>(std::lround(rainLevel * 255.0f));
    snapshot.storm = storm ? 1 : 0;
    for (const auto& [peer, client] : clients) {
        if (!client.welcomed || !client.hasState) continue;
        // Flashes red for others while just hurt.
        const uint8_t flags = static_cast<uint8_t>((client.flags & ~net::kFlagHurt) | (Now() - client.lastHurt < 0.3 ? net::kFlagHurt : 0));
        // (Armour shows only in survival, where it's worn.)
        snapshot.players.push_back({ client.id, client.feet, client.yaw, client.pitch, flags, client.heldTool, client.heldBlock,
                                     static_cast<uint8_t>(Creative(client) ? 0 : client.armour) });
    }
    const auto packet = net::Encode(snapshot);
    for (const auto& [peer, client] : clients)
        if (client.welcomed) net.Send(peer, net::kChannelUnreliable, packet);
}
