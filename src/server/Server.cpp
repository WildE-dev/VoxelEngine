#include "server/Server.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>

#include "server/ServerWorld.h"
#include "server/WorldStorage.h"
#include "world/Collision.h"

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

Collision::AABB BodyAt(const glm::vec3& feet, float shrink = 0.0f) {
    const float half = kPlayerWidth * 0.5f - shrink;
    return { feet - glm::vec3(half, -shrink, half), feet + glm::vec3(half, kPlayerHeight - shrink, half) };
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
    if (storage)
        for (const auto& [pos, held] : storage->GetChests()) chests[pos] = held;
    weatherSeed ^= static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    weatherMode = options.weather;
    raining = options.weather == Options::Weather::Rain;
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
    case net::MsgType::AttackPlayer: {
        net::AttackPlayer m;
        if ((ok = net::Decode(in, m))) HandleAttackPlayer(client, m.player, m.tool);
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
    const bool blocked = !flying && InsideSolid(*world, BodyAt(state.feet, 0.05f));
    client.blockedStates = blocked ? client.blockedStates + 1 : 0;
    if (client.blockedStates >= kBlockedStatesBeforeCorrection) return correct(client.lastValid, "inside solid blocks");

    client.hasState = true;
    client.feet = state.feet;
    client.yaw = state.yaw;
    client.pitch = state.pitch;
    client.flags = state.flags;
    client.heldTool = state.heldTool;
    client.heldBlock = state.heldBlock;
    client.lastStateTime = now;
    if (!blocked) client.lastValid = state.feet;
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
        const bool made = recipe.tool != Tool::None ? (client.tools & Crafting::ToolBit(recipe.tool)) != 0 : (client.armour & Armour::Bit(recipe.armour)) != 0;
        if (made) return SendResources(client); // made already
        count = 1;
    }
    if (Crafting::TimesAffordable(recipe, client.resources) < static_cast<uint32_t>(count)) return SendResources(client); // can't afford
    for (const auto& in : recipe.inputs) client.resources[static_cast<size_t>(in.type)] -= in.units * count;
    if (recipe.tool != Tool::None) client.tools |= Crafting::ToolBit(recipe.tool);
    else if (recipe.armour != ArmourPiece::Count) client.armour |= Armour::Bit(recipe.armour);
    else client.resources[static_cast<size_t>(recipe.output)] += recipe.outputUnits * count;
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
    if (creatures.Attack(creature, eye, tool, now, killed) && killed) {
        std::string spoils;
        // A deer gives venison (one or two).
        if (type == CreatureType::Deer) {
            const uint32_t venison = 1 + static_cast<uint32_t>(tickCount % 2);
            client.resources[static_cast<size_t>(BlockType::RAW_VENISON)] += venison * Block::kUnitsPerBlock;
            client.resources[static_cast<size_t>(BlockType::HIDE)] += Block::kUnitsPerBlock; // and its hide
            SendResources(client);
            spoils = " (+" + std::to_string(venison) + " raw venison, +1 hide)";
        }
        SendChat(client, net::ChatMessage::Kind::Info, std::string("You defeated a ") + SpecOf(type).id + spoils);
    }
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
        Hurt(target, Combat::Damage(tool), "was slain by " + client.name);
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
            m.creatures.push_back({ c.id, c.type, c.feet, c.yaw, static_cast<uint8_t>(now < c.hurtUntil ? net::kCreatureHurt : 0) });
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
        const double needed = Mining::Seconds(block, tool);
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
    // Survival: the block's worth goes to the player.
    if (!Creative(client)) {
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
            const Collision::AABB body = BodyAt(other.feet);
            if (glm::all(glm::lessThan(lo, body.max)) && glm::all(glm::greaterThan(hi, body.min))) return Echo(client, pos);
        }
    }
    world->SetBlock(pos.x, pos.y, pos.z, placed);
    if (type == BlockType::CHEST) chests[pos] = ChestItems{}; // a new chest is empty
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
    case Options::Weather::Clear: raining = false; break;
    case Options::Weather::Rain: raining = true; break;
    case Options::Weather::Auto:
        weatherTimer -= dt;
        if (weatherTimer <= 0.0) {
            raining = !raining;
            weatherTimer = raining ? randomRange(2.0 * 60.0, 5.0 * 60.0) : randomRange(10.0 * 60.0, 25.0 * 60.0);
            Log(raining ? "it starts to rain" : "the rain stops");
        }
        break;
    }
    const float target = raining ? 1.0f : 0.0f;
    const float step = static_cast<float>(dt / 10.0); // fades over ten seconds
    rainLevel = rainLevel < target ? std::min(target, rainLevel + step) : std::max(target, rainLevel - step);
    world->GetWater().SetRaining(rainLevel > 0.5f);
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

    // Dead: back to the spawn point, healed, and briefly safe.
    Broadcast(net::ChatMessage::Kind::Info, client.name + " " + deathMessage);
    client.health = kMaxHealth;
    client.air = Breathing::kMaxAir;
    client.food = Hunger::kMaxFood;
    client.exhaustion = 0.0f;
    client.hasHungerFeet = false;
    client.invulnerableUntil = now + 3.0;
    Teleport(client, SpawnPoint());
    SendHealth(client, true);
}

void Server::TriggerPlate(ClientState& client, const glm::ivec3& plate) {
    int opened = 0;
    const int r = kSluiceReach;
    for (int x = -r; x <= r; x++)
        for (int y = -r; y <= r; y++)
            for (int z = -r; z <= r; z++) {
                Block b;
                const glm::ivec3 p = plate + glm::ivec3(x, y, z);
                if (world->GetBlock(p.x, p.y, p.z, b) && b.type == BlockType::SLUICE) {
                    world->SetBlock(p.x, p.y, p.z, Block());
                    opened++;
                }
            }
    if (opened) {
        SendChat(client, net::ChatMessage::Kind::Info, "*click* Something grinds open overhead...");
        Log(client.name + " opened " + std::to_string(opened) + " sluice gates");
    }
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
    SendResources(client);
    SendHealth(client, false);
}

void Server::HandleOpenChest(ClientState& client, const glm::ivec3& pos, bool open) {
    client.openChest.reset();
    Block block;
    if (open && InReach(client, pos, kChestReach) && world->GetBlock(pos.x, pos.y, pos.z, block) && block.type == BlockType::CHEST) {
        client.openChest = pos;
        chests.try_emplace(pos); // (one from before chests were kept: empty)
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

void Server::RemoveChest(const glm::ivec3& pos, ClientState* breaker) {
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
        bool spiked = false;
        for (int i = 0; i < 4; i++) {
            Block b;
            const glm::ivec3& f = feetCells[i];
            if (world->GetBlock(f.x, f.y, f.z, b)) {
                if (b.type == BlockType::SPIKES) spiked = true;
                if (b.type == BlockType::PRESSURE_PLATE) {
                    auto it = plateReady.find(f);
                    if (it == plateReady.end() || now >= it->second) {
                        plateReady[f] = now + 2.0;
                        TriggerPlate(client, f);
                    }
                }
            }
            const glm::ivec3& u = underCells[i];
            if (world->GetBlock(u.x, u.y, u.z, b) && b.type == BlockType::CRUMBLING_BRICK && !crumbling.count(u)) crumbling[u] = now + kCrumbleDelay;
        }
        if (spiked && now >= client.nextSpikeHit) {
            client.nextSpikeHit = now + kSpikeInterval;
            Hurt(client, kSpikeDamage, "was impaled on spikes");
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
    // Plates' cooldowns that have run out.
    for (auto it = plateReady.begin(); it != plateReady.end();) it = now >= it->second ? plateReady.erase(it) : std::next(it);
}

// --- Tick -------------------------------------------------------------------------

void Server::Tick() {
    tickCount++;
    RunConsoleCommands();
    UpdateSky();
    UpdateTraps();
    UpdateCreatures();
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
    net.Flush();
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
