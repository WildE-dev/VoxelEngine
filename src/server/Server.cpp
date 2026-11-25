#include "server/Server.h"
#include "world/Fire.h"
#include "world/Lava.h"
#include "world/Trees.h"
#include <map>
#include <unordered_set>
#include <fstream>
#include "world/Trades.h"
#include "world/Mining.h"
#include "world/Crafting.h"
#include "world/Smelting.h"
#include <filesystem>
#include "world/BlockData.h"
#include "world/Bodies.h"
#include "world/Dwarves.h"

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
// Movement the server allows, besides falling (blocks a second): survival on foot or in a
// boat (a boat sprints at 9), creative flying (sprinting at 20), and anyone for a moment
// after a knock (kKnockSeconds).
constexpr float kSurvivalSpeed = 11.0f, kCreativeSpeed = 24.0f, kKnockedSpeed = 26.0f;
constexpr float kLaunchedSpeed = 36.0f; // (the hardest air launcher throw, Power::kMaxLaunchSpeed, and a little)
constexpr double kKnockSeconds = 1.5;
// Survival players in the air: how far above where they were last held up they may rise
// (a jump onto a step, a hop out of water), and how long they may stay up without
// coming down below it (a jump takes well under a second).
constexpr float kMaxRise = 1.9f, kKnockedRise = 3.0f;
constexpr double kMaxHover = 2.0;
constexpr int kBlockedStatesBeforeCorrection = 4;

// A player's body (Resources/Data/bodies.json, as the client's Player has it).
const Bodies::Player& Body() { return Bodies::PlayerBody(); }

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
// Bounding down a slope: within this far of the ground and falling slower than this
// (blocks a second), a fall starts counting afresh.
constexpr float kHopHeight = 2.5f, kHopSpeed = 12.0f;
constexpr float kFallGravity = 28.0f; // (as the player's: Player.cpp)
// Arrows: how fast they leave the bow, how fast they fall, what a hit does, and how
// often a bow can shoot.
constexpr float kArrowSpeed = 32.0f, kArrowGravity = 18.0f;
constexpr int kArrowDamage = 5;
constexpr double kShotInterval = 0.6;
// A crossbow's bolt: harder and flatter, but it takes longer to wind.
constexpr double kCrossbowInterval = 1.2;
constexpr float kBoltSpeed = 46.0f;
constexpr int kBoltDamage = 9;
constexpr uint16_t kCreatureShooter = 0xFFFF; // a bolt a dwarf crossbowman shot
// TNT: seconds from lighting to the bang, and how far the blast reaches.
constexpr double kTntFuse = 3.0;
constexpr int kTntRadius = 3;

// A player's body; a crouching player's is shorter (net::kFlagCrouching).
Collision::AABB BodyAt(const glm::vec3& feet, float shrink = 0.0f, bool crouching = false) {
    const float half = Body().width * 0.5f - shrink;
    const float height = crouching ? Body().crouchHeight : Body().height;
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
                if (!world.GetBlock(x, y, z, block)) continue;
                // (As players collide with it; upright slopes are ground.)
                if (Collision::BlockOverlaps(world, glm::ivec3(x, y, z), block, box, false)) return true;
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
    // Blocks' editable details (hardness, tools: what breaking takes), if there's the file.
    {
        std::string error;
        if (std::filesystem::exists(BlockData::kPath) && !BlockData::Load(BlockData::kPath, error)) Log("[data] " + error);
        if (std::filesystem::exists(ItemData::kPath) && !ItemData::Load(ItemData::kPath, error)) Log("[data] " + error); // (items' names, for messages)
        if (std::filesystem::exists(Bodies::kPath) && !Bodies::Load(Bodies::kPath, error)) Log("[data] " + error);
        if (std::filesystem::exists(Crafting::kRecipesPath) && !Crafting::Load(Crafting::kRecipesPath, error)) Log("[data] " + error);
        if (std::filesystem::exists(Smelting::kPath) && !Smelting::Load(Smelting::kPath, error)) Log("[data] " + error);
    }
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
        LoadFallenKings();
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
    if (storage) timeOfDay = storage->GetTimeOfDay(), nights = storage->GetNights();
    defaultMode = storage ? static_cast<net::GameMode>(storage->GetDefaultMode()) : options.defaultMode;
    LoadOperators();
    for (const auto& name : options.operators) SetOperator(name, true);
    creatures.SetShooter([this](const glm::vec3& from, const glm::vec3& dir) {
        arrows.push_back({ from, dir * kArrowSpeed * 0.8f, kCreatureShooter, Now(), -1.0, kArrowDamage });
    });
    if (storage) {
        for (const auto& [pos, held] : storage->GetChests()) chests[pos] = held;
        for (const auto& pos : storage->GetCrops()) crops.insert(pos);
        for (const auto& pos : storage->GetPlacedLeaves()) placedLeaves.insert(pos);
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
        for (const auto& f : storage->GetFluids()) {
            if (f.units == 0 || f.kind == 0 || f.kind >= static_cast<uint8_t>(Fluids::Kind::Count)) continue;
            fluidHeld[f.pos] = static_cast<int>(f.units);
            fluidKind[f.pos] = static_cast<Fluids::Kind>(f.kind);
        }
        for (const auto& a : storage->GetAir()) air[a.pos] = a.units;
        for (const auto& i : storage->GetItems())
            if (const uint32_t id = dropped.Drop(i.item, i.count, i.units, i.position, glm::vec3(0.0f), 0.0f))
                for (auto& d : dropped.All())
                    if (d.id == id) d.age = i.age;
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
    record.inventory = client.inventory;
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
    storage->SetTimeOfDay(TimeOfDayFloat());
    storage->SetNights(nights);
    storage->SetChests({ chests.begin(), chests.end() });
    storage->SetCrops({ crops.begin(), crops.end() });
    // (Placed leaves that are gone now are forgotten; those in unloaded chunks kept.)
    for (auto it = placedLeaves.begin(); it != placedLeaves.end();) {
        Block b;
        if (world->GetBlock(it->x, it->y, it->z, b) && !IsLeavesType(b.type)) it = placedLeaves.erase(it);
        else ++it;
    }
    storage->SetPlacedLeaves({ placedLeaves.begin(), placedLeaves.end() });
    storage->SetSigns({ signs.begin(), signs.end() });
    std::vector<WorldStorage::SavedSignal> savedSignals;
    for (const auto& [pos, b] : signalBlocks)
        savedSignals.push_back({ pos, static_cast<uint8_t>(b.config.kind), static_cast<uint8_t>(b.config.mode), b.config.a, b.config.b, b.config.out,
                                 static_cast<uint8_t>(b.on ? 1 : 0) });
    storage->SetSignals(std::move(savedSignals));
    std::vector<WorldStorage::SavedFluid> savedFluids;
    for (const auto& [pos, units] : fluidHeld)
        if (units > 0) savedFluids.push_back({ pos, static_cast<uint8_t>(fluidKind[pos]), static_cast<uint32_t>(units) });
    storage->SetFluids(std::move(savedFluids));
    std::vector<WorldStorage::SavedAir> savedAir;
    for (const auto& [pos, units] : air)
        if (units > 0.0f) savedAir.push_back({ pos, units });
    storage->SetAir(std::move(savedAir));
    std::vector<WorldStorage::SavedItem> savedItems;
    for (const auto& d : dropped.All()) savedItems.push_back({ d.item, d.count, d.units, d.position, d.age });
    storage->SetItems(std::move(savedItems));
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
    client.lastHeard = Now();
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
    case net::MsgType::Trade: {
        net::Trade m;
        if ((ok = net::Decode(in, m))) HandleTrade(client, m.creature, m.offer);
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
        if ((ok = net::Decode(in, m))) HandleChestMove(client, m.pos, m.item, m.count, m.intoChest != 0);
        break;
    }
    case net::MsgType::InventoryMove: {
        net::InventoryMove m;
        if ((ok = net::Decode(in, m))) HandleInventoryMove(client, m);
        break;
    }
    case net::MsgType::CreativeSlot: {
        net::CreativeSlot m;
        if ((ok = net::Decode(in, m))) HandleCreativeSlot(client, m);
        break;
    }
    case net::MsgType::UseBucket: {
        net::UseBucket m;
        if ((ok = net::Decode(in, m))) HandleUseBucket(client, m);
        break;
    }
    case net::MsgType::DropItem: {
        net::DropItem m;
        if ((ok = net::Decode(in, m))) HandleDropItem(client, m);
        break;
    }
    case net::MsgType::Eat: {
        net::Eat m;
        if ((ok = net::Decode(in, m))) HandleEat(client, m.food);
        break;
    }
    case net::MsgType::Craft: {
        net::Craft m;
        if ((ok = net::Decode(in, m))) HandleCraft(client, m.recipe, m.count, m.tool);
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
    case net::MsgType::Flatten: {
        net::Flatten m;
        if ((ok = net::Decode(in, m))) HandleFlatten(client, m.pos, m.radius);
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
    // One player of a name at a time (whatever its capitals): their saved place and
    // things go by it.
    {
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        // (Unless that one's gone quiet: a connection that dropped and hasn't timed out
        // yet, the same player coming back. It's let go, and they carry on from it.)
        constexpr double kStaleSeconds = 3.0;
        net::PeerId stale{};
        bool replace = false;
        for (auto& [peer, other] : clients) {
            if (&other == &client || !other.welcomed || lower(other.name) != lower(client.name)) continue;
            if (Now() - other.lastHeard < kStaleSeconds) return Kick(client, "someone called " + other.name + " is already playing here: choose another name");
            stale = peer, replace = true;
        }
        if (replace) {
            Log(client.name + " came back while their old connection hung on: letting it go");
            StorePlayer(clients[stale]);
            net.Disconnect(stale);
            clients.erase(stale);
        }
    }
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
            client.inventory = saved->inventory;
            client.flags = saved->flags;
            client.health = std::clamp(saved->health, 1, kMaxHealth);
            client.food = std::clamp(saved->food, 0, Hunger::kMaxFood);
            // (One piece to a part of the body: an older save's extra comes off into
            // their slots.)
            uint8_t off = 0;
            client.armour = Armour::OnePerSlot(saved->armour, off);
            for (int p = 0; p < static_cast<int>(ArmourPiece::Count); p++)
                if (off & Armour::Bit(static_cast<ArmourPiece>(p))) client.inventory.Add(Items::OfArmour(static_cast<ArmourPiece>(p)), 1);
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
    SendInventory(client);
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
    else if (!client.noclip) {
        // Across the ground (falling isn't limited): as fast as the mode allows, faster for
        // a moment after a knock.
        // (Thrown by an air launcher: as fast as the hardest throw, while they fly.)
        const float limit = now < client.launchedUntil ? kLaunchedSpeed : now < client.knockedUntil ? kKnockedSpeed : Creative(client) ? kCreativeSpeed : kSurvivalSpeed;
        const glm::vec2 across(state.feet.x - client.lastValid.x, state.feet.z - client.lastValid.z);
        if (glm::length(across) > limit * dt + kMoveSlack) return correct(client.lastValid, "moved too fast");
    }
    // Survival players fly only as high as a jump above where they were last held up, and
    // don't stay up in the air.
    {
        const bool free = client.noclip || Creative(client);
        const bool crouching = (state.flags & net::kFlagCrouching) != 0;
        bool unknown = false;
        const bool grounded = Collision::GroundHeight(*world, BodyAt(state.feet, 0.05f, crouching), 0.1f, 0.15f, unknown).has_value() || unknown;
        bool held = grounded;
        const Collision::AABB body = BodyAt(state.feet, 0.0f, crouching);
        for (int x = static_cast<int>(std::floor(body.min.x)); x <= static_cast<int>(std::floor(body.max.x)) && !held; x++)
            for (int y = static_cast<int>(std::floor(body.min.y)) - 1; y <= static_cast<int>(std::floor(body.max.y)) && !held; y++)
                for (int z = static_cast<int>(std::floor(body.min.z)); z <= static_cast<int>(std::floor(body.max.z)) && !held; z++) {
                    Block b;
                    held = world->GetBlock(x, y, z, b) && (b.HasWater() || b.IsLava() || b.type == BlockType::LADDER);
                }
        if (free || client.checkedFree || held || !client.hasState || now < client.launchedUntil) {
            client.support = state.feet;
            client.supportAt = now;
        }
        else {
            const float rise = now < client.knockedUntil ? kKnockedRise : kMaxRise;
            // (Back to where they last stood: they get no higher by trying again.)
            if (state.feet.y > client.support.y + rise) return correct(client.support, "flew up");
            if (now - client.supportAt > kMaxHover && state.feet.y > client.support.y - 0.5f) return correct(client.support, "hovering");
        }
        client.checkedFree = free;
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
    client.heldItem = state.heldItem;
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
    // Water (or lava) or a ladder where they are ends a fall; so does a boat, if they have
    // one and there's water under it (what the client says isn't taken on trust).
    Block under;
    const bool wetBelow = world->GetBlock(cell.x, cell.y - 1, cell.z, under) && under.HasWater();
    const bool boat = (state.flags & net::kFlagBoat) && (Creative(client) || client.inventory.Units(ItemType::BOAT) > 0) &&
                      (wetBelow || (world->GetBlock(cell.x, cell.y, cell.z, at) && at.HasWater()));
    const bool swimming = boat || (world->GetBlock(cell.x, cell.y, cell.z, at) && (at.HasWater() || at.IsLava() || at.type == BlockType::LADDER)) ||
                          (world->GetBlock(cell.x, cell.y + 1, cell.z, above) && (above.type == BlockType::LADDER || above.HasWater())); // (a ladder ends a fall too)
    if (reset || swimming || Creative(client)) {
        client.fallPeak = state.feet.y;
        client.falling = false;
        return;
    }
    if (!grounded) {
        // Near the ground and not falling fast (bounding down a slope or a stair of steps,
        // the updates mostly catching you mid-hop): the count starts again from here, so
        // a run down a mountainside isn't one long fall. A real fall is falling fast by
        // the time it nears the ground, and counts in full.
        bool near = false;
        const bool nearGround = Collision::GroundHeight(*world, BodyAt(state.feet, 0.05f, false), 0.05f, kHopHeight, near).has_value();
        if (nearGround && state.velocity.y > -kHopSpeed) {
            client.fallPeak = state.feet.y;
            client.falling = true;
            return;
        }
        client.fallPeak = client.falling ? std::max(client.fallPeak, state.feet.y) : std::max(client.feet.y, state.feet.y);
        client.falling = true;
        client.fallVelocity = state.velocity;
        return;
    }
    float fall = client.falling ? client.fallPeak - state.feet.y : 0.0f;
    // Landing on a slope: what hurts is how hard you hit the surface, not how far you
    // dropped (coming down a mountainside with the slope barely hurts; landing on the
    // level is as before). The surface's slope from the block under the feet.
    if (fall > kSafeFall + 0.5f) {
        const glm::ivec3 under(glm::floor(state.feet - glm::vec3(0.0f, 0.02f, 0.0f)));
        Block ground;
        if (world->GetBlock(under.x, under.y, under.z, ground) && ground.IsPartial()) {
            const float fx = std::clamp(state.feet.x - under.x, 0.25f, 0.75f), fz = std::clamp(state.feet.z - under.z, 0.25f, 0.75f);
            const float dx = (ground.SurfaceHeight(fx + 0.2f, fz) - ground.SurfaceHeight(fx - 0.2f, fz)) / 0.4f;
            const float dz = (ground.SurfaceHeight(fx, fz + 0.2f) - ground.SurfaceHeight(fx, fz - 0.2f)) / 0.4f;
            if (std::abs(dx) + std::abs(dz) > 0.05f) {
                const glm::vec3 normal = glm::normalize(glm::vec3(-dx, 1.0f, -dz));
                const float impact = std::max(0.0f, -glm::dot(normal, client.fallVelocity));
                fall = std::min(fall, impact * impact / (2.0f * kFallGravity)); // (the drop that would hit that hard)
            }
        }
    }
    // A hay bale is soft to land on.
    {
        const glm::ivec3 under(glm::floor(state.feet - glm::vec3(0.0f, 0.05f, 0.0f)));
        Block landed;
        if (world->GetBlock(under.x, under.y, under.z, landed) && landed.type == BlockType::HAY_BALE) fall = 0.0f;
    }
    client.falling = false;
    client.fallPeak = state.feet.y;
    if (fall > kSafeFall + 0.5f) Hurt(client, static_cast<int>(std::lround(fall - kSafeFall)), "fell from a high place", false);
}

float Server::EditReach(const ClientState& client) const {
    return Creative(client) ? kReach : kSurvivalReach;
}

bool Server::InReach(const ClientState& client, const glm::ivec3& pos, float reach) const {
    const glm::vec3 eye = client.feet + glm::vec3(0.0f, Body().eyeHeight, 0.0f);
    return glm::distance(eye, glm::vec3(pos) + glm::vec3(0.5f)) <= reach;
}

void Server::Echo(ClientState& client, const glm::ivec3& pos) {
    const glm::ivec3 chunk = Coords::ChunkOf(pos);
    if (client.sent.count(chunk)) client.pendingCells[chunk].insert(LocalIndex(pos));
}

Tool Server::Usable(const ClientState& client, Tool tool) {
    return Creative(client) || client.inventory.BestTier(tool) >= 0 ? tool : Tool::None;
}

int Server::ToolTier(const ClientState& client, Tool tool) {
    if (tool == Tool::None) return 0;
    return Creative(client) ? Items::kTiers - 1 : std::max(0, client.inventory.BestTier(tool));
}

bool Server::Give(ClientState& client, ItemRef item, uint32_t units) {
    const uint32_t lost = client.inventory.AddUnits(item, units);
    if (lost == 0) return true;
    for (uint32_t left = lost / Items::kUnits; left > 0;) {
        const uint32_t n = std::min(left, Items::kMaxStack);
        left -= n;
        dropped.Drop(item, n, 0, client.feet + glm::vec3(0.0f, 0.5f, 0.0f), dropped.Pop() * 0.5f, DroppedItems::kBlockDelay);
    }
    SendChat(client, net::ChatMessage::Kind::Error, "Your inventory is full: " + std::to_string(lost / Items::kUnits) + " " + Items::Name(item) + " dropped on the ground");
    return false;
}

void Server::HandleCraft(ClientState& client, int recipeIndex, int count, Tool upgrade) {
    const auto& recipes = Crafting::Recipes();
    if (Creative(client) || recipeIndex < 0 || recipeIndex >= static_cast<int>(recipes.size())) return;
    const Recipe& recipe = recipes[recipeIndex];
    Inventory& inv = client.inventory;
    if (Crafting::TimesAffordable(recipe, inv) < static_cast<uint32_t>(count)) return SendInventory(client); // can't afford
    if (recipe.Upgrade()) {
        // The tool in their hand, one tier up (in the same slot).
        const ItemId from = Items::OfTool(upgrade, recipe.steelUpgrade ? 1 : 0);
        auto slot = std::find_if(inv.slots.begin(), inv.slots.end(), [&](const Stack& st) { return !st.Empty() && st.item == from; });
        if (upgrade == Tool::None || slot == inv.slots.end()) {
            SendChat(client, net::ChatMessage::Kind::Error, recipe.steelUpgrade ? "Hold an iron tool to make it steel" : "Hold a stone tool to make it iron");
            return SendInventory(client);
        }
        for (const auto& in : recipe.inputs) inv.TakeUnits(in.item, in.units);
        slot->item = Items::OfTool(upgrade, recipe.steelUpgrade ? 2 : 1);
        if (recipe.ironUpgrade) Award(client, Achievement::IronAge);
        return SendInventory(client);
    }
    // What it makes must fit (once what it takes is gone).
    Inventory after = inv;
    for (const auto& in : recipe.inputs) after.TakeUnits(in.item, in.units * count);
    bool fits = true;
    // Armour goes straight on if that slot's free; the rest into the inventory.
    const bool wear = recipe.armour != ArmourPiece::Count && Armour::InSlot(client.armour, Armour::SlotOf(recipe.armour)) == ArmourPiece::Count;
    if (recipe.tool != Tool::None) fits = after.Add(Items::OfTool(recipe.tool), count) == 0;
    else if (recipe.armour != ArmourPiece::Count) fits = after.Add(Items::OfArmour(recipe.armour), static_cast<uint32_t>(count) - (wear ? 1 : 0)) == 0;
    else fits = after.AddUnits(recipe.output, recipe.outputUnits * count) == 0;
    if (!fits) {
        SendChat(client, net::ChatMessage::Kind::Error, "No room in your inventory for that");
        return SendInventory(client);
    }
    inv = std::move(after);
    if (wear) client.armour |= Armour::Bit(recipe.armour);
    if (recipe.tool != Tool::None) Award(client, Achievement::Toolmaker);
    if (recipe.output == ItemType::COOKED_VENISON) Award(client, Achievement::Cook);
    if (recipe.output == ItemType::BREAD) Award(client, Achievement::Baker);
    if (recipe.output == BlockType::TURBINE) Award(client, Achievement::FullSteam);
    if ((client.armour & (Armour::Bit(ArmourPiece::Cap) | Armour::Bit(ArmourPiece::Boots))) == (Armour::Bit(ArmourPiece::Cap) | Armour::Bit(ArmourPiece::Boots)) && Armour::InSlot(client.armour, BodySlot::Body) != ArmourPiece::Count) Award(client, Achievement::Armoured);
    SendInventory(client);
}

Inventory Server::ArmourBox(uint8_t worn) {
    Inventory box(static_cast<size_t>(BodySlot::Count));
    for (int s = 0; s < static_cast<int>(BodySlot::Count); s++)
        if (const ArmourPiece p = Armour::InSlot(worn, static_cast<BodySlot>(s)); p != ArmourPiece::Count) box.slots[s] = { Items::OfArmour(p), 1 };
    return box;
}

uint8_t Server::WornFrom(const Inventory& box) {
    uint8_t worn = 0;
    for (const auto& st : box.slots)
        if (!st.Empty() && Items::IsArmour(st.item)) worn |= Armour::Bit(Items::ArmourOf(st.item));
    return worn;
}

void Server::HandleInventoryMove(ClientState& client, const net::InventoryMove& m) {
    // The armour slots as a little inventory of their own (a slot per part of the body),
    // put back after.
    Inventory armourBox = ArmourBox(client.armour);
    Inventory* chest = (m.fromBox == net::Box::Chest || m.toBox == net::Box::Chest) ? OpenChestOf(client) : nullptr;
    auto box = [&](net::Box b) -> Inventory* {
        return b == net::Box::Player ? &client.inventory : b == net::Box::Armour ? &armourBox : chest;
    };
    // What may go in a slot: an armour slot takes only what's worn there.
    auto fits = [&](net::Box b, size_t slot, ItemId item) {
        return b != net::Box::Armour || (Items::IsArmour(item) && static_cast<size_t>(Armour::SlotOf(Items::ArmourOf(item))) == slot);
    };
    Inventory* src = box(m.fromBox);
    Inventory* dst = box(m.toBox);
    if (!src || !dst || m.fromSlot >= src->slots.size()) return SendInventory(client), SendChest(client);
    Stack& from = src->slots[m.fromSlot];
    if (from.Empty()) return SendInventory(client), SendChest(client);
    const uint32_t n = m.count == 0 ? from.count : std::min<uint32_t>(m.count, from.count);
    const ItemId item = from.item;
    const uint32_t max = Items::MaxStack(item);
    if (m.toSlot == net::kAnySlot) {
        // Wherever it fits: armour onto its slot; within their own slots, from the hotbar
        // into the rest and back; otherwise anywhere in that box.
        size_t begin = 0, end = dst->slots.size();
        if (m.toBox == net::Box::Armour) {
            if (!Items::IsArmour(item)) return SendInventory(client);
            begin = static_cast<size_t>(Armour::SlotOf(Items::ArmourOf(item))), end = begin + 1;
            // (Something else worn there: they swap.)
            if (!dst->slots[begin].Empty() && dst->slots[begin].item != item && n == 1) {
                std::swap(from, dst->slots[begin]);
                begin = end; // (done)
            }
        }
        else if (src == dst) {
            if (m.fromSlot < Inventory::kHotbarSlots) begin = Inventory::kHotbarSlots;
            else end = Inventory::kHotbarSlots;
        }
        uint32_t left = n;
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = begin; i < end && left > 0; i++) {
                Stack& to = dst->slots[i];
                if (&to == &from) continue;
                if (pass == 0 && !to.Empty() && to.item == item && to.count < max) {
                    const uint32_t k = std::min(left, max - to.count);
                    to.count = static_cast<uint8_t>(to.count + k), left -= k;
                }
                else if (pass == 1 && to.Empty()) {
                    const uint32_t k = std::min(left, max);
                    to = { item, static_cast<uint8_t>(k) }, left -= k;
                }
            }
        from.count = static_cast<uint8_t>(from.count - (n - left));
        if (from.count == 0) from = {};
    }
    else {
        if (m.toSlot >= dst->slots.size()) return SendInventory(client), SendChest(client);
        Stack& to = dst->slots[m.toSlot];
        if (&to == &from) return SendInventory(client);
        if (to.Empty() || to.item == item) {
            if (!fits(m.toBox, m.toSlot, item)) return SendInventory(client);
            const uint32_t k = std::min(n, max - (to.Empty() ? 0u : to.count));
            to = { item, static_cast<uint8_t>((to.Empty() ? 0 : to.count) + k) };
            from.count = static_cast<uint8_t>(from.count - k);
            if (from.count == 0) from = {};
        }
        else {
            // Something else there: a whole stack swaps with it.
            if (n != from.count || !fits(m.toBox, m.toSlot, item) || !fits(m.fromBox, m.fromSlot, to.item)) return SendInventory(client);
            std::swap(from, to);
        }
    }
    client.armour = WornFrom(armourBox);
    client.inventory.Trim(Inventory::kPlayerSlots);
    if (chest) chest->Trim(Inventory::kChestSlots);
    if ((client.armour & (Armour::Bit(ArmourPiece::Cap) | Armour::Bit(ArmourPiece::Boots))) == (Armour::Bit(ArmourPiece::Cap) | Armour::Bit(ArmourPiece::Boots)) && Armour::InSlot(client.armour, BodySlot::Body) != ArmourPiece::Count) Award(client, Achievement::Armoured);
    SendInventory(client);
    if (chest) SendChestToViewers(*client.openChest);
}

void Server::HandleCreativeSlot(ClientState& client, const net::CreativeSlot& m) {
    if (!Creative(client) || m.slot >= client.inventory.slots.size()) return SendInventory(client);
    if (m.item == Items::kNone || m.count == 0) client.inventory.slots[m.slot] = {};
    else client.inventory.slots[m.slot] = { m.item, static_cast<uint8_t>(std::min<uint32_t>(m.count, Items::MaxStack(m.item))) };
    SendInventory(client);
}

void Server::HandleAttack(ClientState& client, uint16_t creature, Tool tool) {
    tool = Usable(client, tool);
    const double now = Now();
    if (now < client.nextAttack) return;
    client.nextAttack = now + Combat::kCooldown * 0.8; // (a little slack for timing)
    bool killed = false;
    const glm::vec3 eye = client.feet + glm::vec3(0.0f, Body().eyeHeight, 0.0f);
    const CreatureSystem::Creature* target = creatures.Find(creature);
    const CreatureType type = target ? target->type : CreatureType::Count;
    const bool raider = target && target->raider;
    const bool landed = creatures.Attack(creature, eye, tool, ToolTier(client, tool), now, killed);
    if (landed && !killed) creatures.Provoke(creature, client.id, now);
    if (landed && killed) {
        // A deer gives venison (one or two).
        if (type == CreatureType::Deer) {
            const uint32_t venison = 1 + static_cast<uint32_t>(tickCount % 2);
            Give(client, ItemType::RAW_VENISON, venison * Block::kUnitsPerBlock);
            Give(client, ItemType::HIDE, Block::kUnitsPerBlock); // and its hide
            SendInventory(client);
        }
        // A crossbowman leaves some of its bolts (arrows).
        if (type == CreatureType::DwarfCrossbow) {
            Give(client, ItemType::ARROW, 2 * Block::kUnitsPerBlock);
            SendInventory(client);
        }
        if (type == CreatureType::DwarfKing) KingSlain(client);
        if (type == CreatureType::ForgeGolem) GolemSlain(client);
        if (raider) Award(client, Achievement::Defender);
        if (type == CreatureType::Deer) Award(client, Achievement::Hunter);
        else if (SpecOf(type).hostile) Award(client, Achievement::DwarfSlayer);
    }
}

void Server::SendTradePrices(ClientState& client, uint16_t trader) {
    const auto& offers = Trades::Offers();
    tradePressure.resize(offers.size(), 0);
    net::TradePrices m;
    m.creature = trader;
    for (size_t i = 0; i < offers.size(); i++) m.give.push_back(static_cast<uint16_t>(Trades::Price(offers[i], tradePressure[i])));
    net.Send(client.peer, net::kChannelReliable, net::Encode(m));
}

void Server::HandleInteract(ClientState& client, uint16_t creature) {
    const CreatureSystem::Creature* c = creatures.Find(creature);
    // A trader: its prices (the trade window opens on the player's side).
    if (c && c->type == CreatureType::DwarfTrader) return SendTradePrices(client, creature);
    if (!c || c->type != CreatureType::Wolf || c->owner != 0) return;
    const glm::vec3 eye = client.feet + glm::vec3(0.0f, Body().eyeHeight, 0.0f);
    const Collision::AABB box = CreatureSystem::Box(*c);
    if (glm::distance(eye, glm::clamp(eye, box.min, box.max)) > Combat::kReach + 0.5f) return;
    // Raw venison in hand tames it.
    if (client.heldItem != Items::Of(ItemType::RAW_VENISON) || (!Creative(client) && client.inventory.Units(ItemType::RAW_VENISON) < Block::kUnitsPerBlock)) return;
    if (!creatures.Tame(creature, client.id)) return;
    if (!Creative(client)) {
        client.inventory.TakeUnits(ItemType::RAW_VENISON, Block::kUnitsPerBlock);
        SendInventory(client);
    }
    SendChat(client, net::ChatMessage::Kind::Info, "Wolf tamed");
    Award(client, Achievement::BestFriend);
}

void Server::KingSlain(ClientState& client) {
    Give(client, ItemType::STEEL_INGOT, 6 * Block::kUnitsPerBlock);
    Give(client, ItemType::IRON_INGOT, 8 * Block::kUnitsPerBlock);
    SendInventory(client);
    SendChat(client, net::ChatMessage::Kind::Info, "Hoard: +6 steel ingots, +8 iron ingots");
    Award(client, Achievement::Kingslayer);
    for (auto& [peer, other] : clients)
        if (other.welcomed) SendChat(other, net::ChatMessage::Kind::Info, client.name + " has defeated a Dwarf King");
}

void Server::GolemSlain(ClientState& client) {
    Give(client, ItemType::FORGE_CORE, Block::kUnitsPerBlock);
    Give(client, ItemType::STEEL_INGOT, 3 * Block::kUnitsPerBlock);
    SendInventory(client);
    SendChat(client, net::ChatMessage::Kind::Info, "+1 forge core, +3 steel ingots");
    Award(client, Achievement::GolemBreaker);
    for (auto& [peer, other] : clients)
        if (other.welcomed) SendChat(other, net::ChatMessage::Kind::Info, client.name + " has defeated a forge golem");
}

void Server::LoadFallenKings() {
    if (options.worldDirectory.empty()) return;
    std::ifstream in(std::filesystem::path(options.worldDirectory) / "kings.txt");
    std::unordered_set<int64_t> fallen;
    int x, z;
    while (in >> x >> z) fallen.insert(CreatureSystem::SiteKey({ x, 0, z }));
    creatures.SetFallenKings(fallen);
    fallenKingsSaved = fallen.size();
}

void Server::SaveFallenKings() {
    fallenKingsSaved = creatures.FallenKings().size();
    if (options.worldDirectory.empty()) return;
    std::ofstream out(std::filesystem::path(options.worldDirectory) / "kings.txt");
    out << "# Strongholds whose Dwarf King has fallen: x z of each hall's centre\n";
    for (int64_t key : creatures.FallenKings()) out << static_cast<int32_t>(key >> 32) << ' ' << static_cast<int32_t>(static_cast<uint32_t>(key)) << '\n';
}

void Server::HandleTrade(ClientState& client, uint16_t trader, uint8_t offerIndex) {
    const double now = Now();
    const CreatureSystem::Creature* c = creatures.Find(trader);
    if (!c || c->type != CreatureType::DwarfTrader) return;
    if (glm::distance(c->feet, client.feet) > Trades::kReach) return SendChat(client, net::ChatMessage::Kind::Error, "Come closer to the trader to trade");
    if (creatures.AngryAt(*c, client.id, now)) return SendChat(client, net::ChatMessage::Kind::Error, "The trader won't deal with you, after what you did here");
    const auto& offers = Trades::Offers();
    if (offerIndex >= offers.size()) return;
    const Trades::Offer& o = offers[offerIndex];
    auto name = [](ItemId item) { return Items::Name(item); };
    tradePressure.resize(offers.size(), 0);
    const int price = Trades::Price(o, tradePressure[offerIndex]);
    const uint32_t cost = static_cast<uint32_t>(price) * Block::kUnitsPerBlock;
    if (!Creative(client) && client.inventory.Units(o.give) < cost)
        return SendChat(client, net::ChatMessage::Kind::Error, "You need " + std::to_string(price) + " " + name(o.give) + " for that");
    const glm::vec3 at = c->feet; // (c may move once a creature is added)
    if (o.hire) {
        if (creatures.HiredBy(client.id) >= Trades::kMaxHired)
            return SendChat(client, net::ChatMessage::Kind::Error, "You already have " + std::to_string(Trades::kMaxHired) + " guards");
        glm::vec2 toward(client.feet.x - at.x, client.feet.z - at.z);
        toward = glm::length(toward) > 1e-3f ? glm::normalize(toward) : glm::vec2(1.0f, 0.0f);
        if (!creatures.Hire(at + glm::vec3(toward.x, 0.2f, toward.y), client.id, now, Trades::kHireSeconds))
            return SendChat(client, net::ChatMessage::Kind::Error, "No guard is free just now");
        SendChat(client, net::ChatMessage::Kind::Info, "Guard hired (10 minutes)");
    }
    else {
        // (What they pay is gone first, so it may make the room.)
        Inventory after = client.inventory;
        if (!Creative(client)) after.TakeUnits(o.give, cost);
        if (after.AddUnits(o.get, static_cast<uint32_t>(o.getCount) * Block::kUnitsPerBlock) != 0)
            return SendChat(client, net::ChatMessage::Kind::Error, "You have no room for " + std::to_string(o.getCount) + " " + name(o.get));
        client.inventory = std::move(after); // (paid, and given)
        SendChat(client, net::ChatMessage::Kind::Info,
                 "Traded " + std::to_string(price) + " " + name(o.give) + " for " + std::to_string(o.getCount) + " " + name(o.get));
    }
    if (o.hire && !Creative(client)) client.inventory.TakeUnits(o.give, cost);
    SendInventory(client);
    Award(client, Achievement::Merchant);
    // The price goes up (for everyone), and they're told the new prices.
    tradePressure[offerIndex] = std::min(tradePressure[offerIndex] + 1, Trades::kMaxPressure);
    SendTradePrices(client, trader);
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
        const glm::vec3 eye = client.feet + glm::vec3(0.0f, Body().eyeHeight, 0.0f);
        const Collision::AABB body = Bodies::PlayerBox(target.feet, (target.flags & net::kFlagCrouching) != 0);
        const glm::vec3 lo = body.min, hi = body.max;
        if (glm::distance(eye, glm::clamp(eye, lo, hi)) > Combat::kReach + 0.5f) return;
        // Knocked back, away from the hitter and a little up (as creatures are). The
        // target moves itself, so it's told to; a slain player respawns instead.
        const double before = target.invulnerableUntil;
        Hurt(target, Combat::Damage(tool, ToolTier(client, tool)), "was slain by " + client.name);
        if (target.health > 0 && target.lastHurt >= now && target.invulnerableUntil == before) {
            glm::vec2 away(target.feet.x - client.feet.x, target.feet.z - client.feet.z);
            away = glm::length(away) > 1e-3f ? glm::normalize(away) : glm::vec2(1.0f, 0.0f);
            net.Send(target.peer, net::kChannelReliable, net::Encode(net::Knockback{ glm::vec3(away.x * 7.0f, 5.5f, away.y * 7.0f) }));
            target.knockedUntil = Now() + kKnockSeconds;
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
    if (creatures.FallenKings().size() != fallenKingsSaved) SaveFallenKings();
    // Trade prices ease back.
    if (now >= nextPriceRecovery) {
        nextPriceRecovery = now + Trades::kPriceRecovery;
        for (int& p : tradePressure) p = std::max(0, p - 1);
    }
    for (uint16_t owner : creatures.TakeEndedHires())
        for (auto& [peer, client] : clients)
            if (client.id == owner) SendChat(client, net::ChatMessage::Kind::Info, "Your guard went home");
}

void Server::SendCreatures() {
    const double now = Now();
    constexpr float kSendRange = 72.0f;
    for (auto& [peer, client] : clients) {
        if (!client.welcomed) continue;
        // The dwarf sites round them, for their map: told again when they cross into
        // another region (or a king falls).
        const glm::ivec2 region(static_cast<int>(std::floor(client.feet.x / 64.0f)), static_cast<int>(std::floor(client.feet.z / 64.0f)));
        if (client.hasState && (region != client.sitesRegion || client.sitesFallen != creatures.FallenKings().size())) {
            client.sitesRegion = region;
            client.sitesFallen = creatures.FallenKings().size();
            net::Sites sites;
            for (const auto& s : Dwarves::SitesNear(world->GetTerrain(), client.feet, 480.0f))
                sites.sites.push_back({ static_cast<uint8_t>(s.kind), static_cast<uint8_t>(creatures.FallenKings().count(CreatureSystem::SiteKey(s.centre)) ? 1 : 0),
                                        s.centre });
            if (client.bed) sites.hasBed = 1, sites.bed = *client.bed;
            net.Send(peer, net::kChannelReliable, net::Encode(sites));
        }
        net::CreatureSnapshot m;
        m.tick = tickCount;
        for (const auto& c : creatures.All()) {
            if (glm::distance(c.feet, client.feet) > kSendRange) continue;
            m.creatures.push_back({ c.id, c.type, c.feet, c.yaw,
                                    static_cast<uint8_t>((now < c.hurtUntil ? net::kCreatureHurt : 0) | (c.owner != 0 ? net::kCreatureTame : 0) |
                                                         (c.act == 1 && now - c.actedAt < CreatureSystem::kAttackShown ? net::kCreatureAttacking : 0) |
                                                         (c.act == 2 && now - c.actedAt < CreatureSystem::kWorkShown ? net::kCreatureWorking : 0)),
                                    static_cast<uint8_t>(std::clamp(255 * c.health / std::max(1, SpecOf(c.type).maxHealth), 0, 255)) });
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
        const double needed = Mining::Seconds(block, tool, ToolTier(client, tool));
        const bool started = client.mining && client.miningPos == pos && client.miningTool == tool;
        const double mined = started ? Now() - client.miningSince : 0.0;
        if (needed > 0.0 && mined < needed * (1.0 - Mining::kSlackShare) - Mining::kSlackSeconds) {
            if (options.log && started) Log(client.name + " broke a block too soon");
            return Echo(client, pos);
        }
        client.mining = false;
    }
    // Part of a formed structure: it comes apart, and the block broken is what it was.
    if (block.type == BlockType::MULTIBLOCK || block.type == BlockType::MULTIBLOCK_PART) block = TakeApart(pos);
    // Breaking a waterlogged block leaves its water behind.
    world->SetBlock(pos.x, pos.y, pos.z, block.HasWater() ? Block::Water(block.WaterVolume()) : Block());
    if (IsStorageType(block.type)) RemoveChest(pos, &client);
    if (block.type == BlockType::CROP) crops.erase(pos);
    if (block.type == BlockType::SIGN && signs.erase(pos)) BroadcastSign(pos);
    // A log: with an axe, the logs straight above come down with it (each giving its
    // worth); and leaves that nothing holds up any more start to fall away.
    if (Trees::IsLog(block)) {
        std::vector<glm::ivec3> gone{ pos };
        if (tool == Tool::Axe)
            for (const auto& p : Trees::LogsAbove(*world, pos)) {
                Block log;
                if (!world->GetBlock(p.x, p.y, p.z, log)) break;
                world->SetBlock(p.x, p.y, p.z, log.HasWater() ? Block::Water(log.WaterVolume()) : Block());
                if (!Creative(client)) dropped.DropUnits(Items::ResourceOf(log.type), static_cast<uint32_t>(log.ResourceUnits()), p, client.id);
                gone.push_back(p);
            }
        LoosenLeaves(gone);
    }
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
        if (seeds) dropped.DropUnits(ItemType::SEEDS, seeds * Block::kUnitsPerBlock, pos, client.id);
        // Leaves now and then drop a sapling (a pine's, from pine needles).
        if (IsLeavesType(block.type) && roll > 0.88f)
            dropped.DropUnits(block.type == BlockType::PINE_LEAVES ? BlockType::PINE_SAPLING : BlockType::SAPLING, Block::kUnitsPerBlock, pos, client.id);
    }
    // Survival: the block's worth drops where it was (and flies to them).
    if (!Creative(client)) {
        Award(client, Achievement::FirstBlock);
        if (block.type == BlockType::IRON_ORE) Award(client, Achievement::Miner);
        if (block.type == BlockType::RIPE_WHEAT) Award(client, Achievement::Farmer);
        if (!IsCreativeOnlyType(block.type))
            dropped.DropUnits(Items::ResourceOf(block.type), static_cast<uint32_t>(block.ResourceUnits()), pos, client.id); // (pebbles are stone)
    }
}

void Server::HandlePlace(ClientState& client, const glm::ivec3& pos, BlockType type, int height, int faceAxis) {
    // Refused: the client predicted it, so send back what's really there (the cells, and
    // in survival the count it took the cost from).
    auto refuse = [&] {
        if (!Creative(client)) SendInventory(client);
        Echo(client, pos);
        if (type == BlockType::DOOR) Echo(client, pos + glm::ivec3(0, 1, 0)); // (it predicted the upper half too)
    };
    // Something that can be put down (not a part of something bigger), with room for it (a
    // door's upper half too; seeds on dirt or grass): BlockAccess::CanPlace, which the
    // client checks too before predicting it.
    if (type == BlockType::AIR || type >= BlockType::COUNT || !InReach(client, pos, EditReach(client)) || !world->CanPlace(pos, type)) return refuse();
    if (IsCreativeOnlyType(type) && !Creative(client)) return refuse();
    const Block placed = Block::Placed(type, height, faceAxis);
    if (IsLeverType(type) && !world->LeverHeld(pos, placed)) return refuse(); // (only on a solid block)
    // Survival: it costs its worth, which the player must have. Water can't be
    // collected (only moved, with pumps), so it can't be placed either.
    const uint32_t cost = static_cast<uint32_t>(placed.ResourceUnits());
    if (!Creative(client) && (client.inventory.Units(Items::ResourceOf(type)) < cost || placed.IsLiquid())) return refuse();
    // No solid blocks inside a walking player (water is fine).
    if (placed.IsFullBlock()) {
        const glm::vec3 lo(pos), hi = lo + glm::vec3(1.0f);
        for (const auto& [peer, other] : clients) {
            if (!other.welcomed || ((other.flags & net::kFlagFlying) && other.noclip)) continue;
            const Collision::AABB body = BodyAt(other.feet, 0.0f, (other.flags & net::kFlagCrouching) != 0);
            if (glm::all(glm::lessThan(lo, body.max)) && glm::all(glm::greaterThan(hi, body.min))) return refuse();
        }
    }
    world->SetBlock(pos.x, pos.y, pos.z, placed);
    if (IsStorageType(type)) chests[pos] = Inventory(Inventory::kChestSlots); // a new chest is empty
    if (type == BlockType::CROP || type == BlockType::SAPLING || type == BlockType::PINE_SAPLING) crops.insert(pos);
    if (IsLeavesType(type)) placedLeaves.insert(pos); // (it stays when a tree's logs go)
    if (type == BlockType::DOOR) world->SetBlock(pos.x, pos.y + 1, pos.z, placed); // its upper half (part of the one door, facing the same way)
    if (!Creative(client)) {
        client.inventory.TakeUnits(Items::ResourceOf(type), cost);
        SendInventory(client);
    }
}

void Server::HandleChisel(ClientState& client, const net::EditChisel& chisel) {
    const glm::ivec3& p = chisel.pos;
    Block block;
    // Needs the chisel (creative has every tool), and moves the corner by the step chosen.
    if (Usable(client, Tool::Chisel) != Tool::Chisel || !InReach(client, p, EditReach(client)) || !world->GetBlock(p.x, p.y, p.z, block) || !block.CanChisel())
        return Echo(client, p);
    const Block after = block.ChiselledFrom(chisel.dir, chisel.edge, chisel.raise ? chisel.step : -chisel.step);
    // Survival: chiselling off gives back what it takes away; building up costs it.
    if (!Creative(client)) {
        const ItemId type = Items::ResourceOf(block.type);
        const int change = after.ResourceUnits() - block.ResourceUnits(); // + costs, - gives
        const bool ok = change > 0 ? client.inventory.TakeUnits(type, static_cast<uint32_t>(change))
                                   : client.inventory.FitsUnits(type, static_cast<uint32_t>(-change));
        if (!ok) {
            if (change < 0) SendChat(client, net::ChatMessage::Kind::Error, "Your inventory is full");
            SendInventory(client);
            return Echo(client, p);
        }
        if (change < 0) client.inventory.AddUnits(type, static_cast<uint32_t>(-change));
        SendInventory(client);
    }
    world->SetBlock(p.x, p.y, p.z, after);
}

void Server::HandleFlatten(ClientState& client, const glm::ivec3& pos, int radius) {
    radius = std::clamp(radius, 1, net::kMaxFlattenRadius);
    Block clicked;
    if (Usable(client, Tool::Shovel) != Tool::Shovel || !InReach(client, pos, EditReach(client)) || !world->GetBlock(pos.x, pos.y, pos.z, clicked)) return;
    // Soft ground: what a shovel digs (dirt, grass, sand, snow, gravel), whole or shaped.
    auto soft = [](const Block& b) { return !b.IsAir() && !b.IsLiquid() && !b.IsPlant() && !IsModelType(b.type) && Mining::BestTool(b.type) == Tool::Shovel; };
    if (!soft(clicked) || clicked.ShapeDir() != 0) return;
    // The level: the clicked block's top (the middle of its corners, for a shaped one), in
    // eighths from the bottom of the world's rows.
    int top = 8;
    if (!clicked.IsFullBlock()) {
        int sum = 0;
        for (int e = 0; e < 4; e++) sum += clicked.edgeData.GetTopY(e);
        top = (sum + 2) / 4;
    }
    const int level = pos.y * 8 + top;
    struct Change {
        glm::ivec3 at;
        Block to;
    };
    std::vector<Change> changes;
    std::map<ItemId, long> cost; // units, by what they're made of: + taken, - given back
    for (int dx = -radius; dx <= radius; dx++)
        for (int dz = -radius; dz <= radius; dz++)
            for (int y = pos.y - 1; y <= pos.y + 2; y++) {
                const glm::ivec3 at(pos.x + dx, y, pos.z + dz);
                Block b;
                if (!world->GetBlock(at.x, at.y, at.z, b) || b.HasWater() || b.IsLava()) continue;
                const bool open = b.IsAir() || b.IsReplaceablePlant();
                if (!open && !(soft(b) && b.ShapeDir() == 0)) continue; // (stone, wood, buildings: left be)
                const int fill = std::clamp(level - y * 8, 0, 8);
                // Filling an empty cell: with what was clicked (soft ground keeps its own kind).
                const BlockType kind = open ? clicked.type : b.type;
                Block to;
                if (fill == 8) to = Block(kind);
                else if (fill > 0) {
                    EdgeData edges;
                    for (int e = 0; e < 4; e++) edges.SetTopY(e, static_cast<uint8_t>(fill));
                    to = Block(kind, edges);
                }
                if (to == b || (open && fill == 0)) continue;
                const int before = open ? 0 : b.ResourceUnits(), after = to.ResourceUnits();
                if (before != after) cost[open ? Items::ResourceOf(to.type) : Items::ResourceOf(b.type)] += after - before;
                changes.push_back({ at, to });
            }
    if (changes.empty()) return;
    if (!Creative(client)) {
        Inventory after = client.inventory;
        for (const auto& [type, units] : cost) {
            if (units > 0 && !after.TakeUnits(type, static_cast<uint32_t>(units))) {
                SendChat(client, net::ChatMessage::Kind::Error, "Not enough " + Items::Id(type) + " to fill it in");
                return SendInventory(client);
            }
        }
        for (const auto& [type, units] : cost)
            if (units < 0 && after.AddUnits(type, static_cast<uint32_t>(-units)) != 0) {
                SendChat(client, net::ChatMessage::Kind::Error, "Your inventory is full");
                return SendInventory(client);
            }
        client.inventory = std::move(after);
        SendInventory(client);
    }
    // (Bottom up, so what's filled in rests on what's under it.)
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.at.y < b.at.y; });
    for (const auto& c : changes) world->SetBlock(c.at.x, c.at.y, c.at.z, c.to);
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
    // Dry spells: long enough, and it's a drought (until it rains).
    dryFor = raining ? 0.0 : dryFor + dt;
    const bool dryNow = dryFor >= kDroughtDays * std::max(1.0f, options.dayLengthSeconds);
    if (dryNow != drought) {
        drought = dryNow;
        Broadcast(net::ChatMessage::Kind::Info, drought ? "No rain for days: a drought. The springs run low." : "The drought breaks.");
    }
    world->GetWater().SetSpringFlow(SpringFlow());
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
    // It sets fire to where it strikes (it dies down soon on bare ground; in a forest...).
    Ignite(*top);
    Scorch(*top, 2, 0.5f);
    // Anyone close by is struck: three hearts (armour helps).
    for (ClientState* p : players)
        if (glm::distance(p->feet, glm::vec3(*top) + glm::vec3(0.5f, 0.0f, 0.5f)) < 3.0f) Hurt(*p, 6, "was struck by lightning");
}

// --- Traps and health -------------------------------------------------------------

void Server::SendMode(ClientState& client) {
    net.Send(client.peer, net::kChannelReliable, net::Encode(net::GameModeIs{ client.mode }));
}

void Server::SendInventory(ClientState& client) {
    net::InventoryIs m;
    m.items = client.inventory;
    m.armour = client.armour;
    net.Send(client.peer, net::kChannelReliable, net::Encode(m));
}

glm::vec3 Server::SpawnPoint() const {
    return glm::vec3(7.5f, static_cast<float>(terrain.ColumnAt(7, 7).FirstOpenRow()) + 1.0f, 7.5f);
}

namespace {
// How full an oxygen tank is, as sent: 0 (empty) to 255 (full).
int TankByte(float tank) { return static_cast<int>(std::clamp(std::lround(255.0f * tank / Breathing::kTankSeconds), 0L, 255L)); }
} // namespace

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
    client.sentTank = TankByte(client.tank);
    client.sentOnTank = client.onTank;
    m.tank = static_cast<uint8_t>(client.sentTank);
    m.onTank = client.onTank ? 1 : 0;
    client.sentBurning = Now() < client.burningUntil;
    m.burning = client.sentBurning ? 1 : 0;
    net.Send(client.peer, net::kChannelReliable, net::Encode(m));
}

void Server::Hurt(ClientState& client, int amount, const std::string& deathMessage, bool armourHelps) {
    const double now = Now();
    if (now < client.invulnerableUntil || Creative(client)) return; // nothing hurts in creative
    if (armourHelps) {
        amount = Armour::Reduce(amount, client.armour);
        // Crouched behind a shield: half of it blocked.
        if ((client.armour & Armour::Bit(ArmourPiece::Shield)) && (client.flags & net::kFlagCrouching)) amount = (amount + 1) / 2;
    }
    client.health = std::max(0, client.health - amount);
    client.lastHurt = now;
    client.nextRegen = now + kRegenDelay;
    if (client.health > 0) return SendHealth(client, true);

    // Dead: what they carry is left where they died, then back to the spawn
    // point, healed, and briefly safe.
    Broadcast(net::ChatMessage::Kind::Info, client.name + " " + deathMessage);
    DropOnDeath(client);
    client.health = kMaxHealth;
    client.air = Breathing::kMaxAir;
    client.food = Hunger::kMaxFood;
    client.exhaustion = 0.0f;
    client.hasHungerFeet = false;
    client.burningUntil = 0.0; // (they don't come back still alight)
    client.invulnerableUntil = now + 3.0;
    std::string why;
    Teleport(client, RespawnPoint(client, why));
    if (!why.empty()) SendChat(client, net::ChatMessage::Kind::Info, why);
    SendHealth(client, true);
}

void Server::DropOnDeath(ClientState& client) {
    if (!options.dropOnDeath || Creative(client)) return;
    // Everything they carry and wear goes into the chest (blocks and items, their offcuts,
    // tools, armour); they come back with nothing, and can go and fetch it.
    Inventory dropped(Inventory::kChestSlots);
    for (const Stack& st : client.inventory.slots)
        if (!st.Empty()) dropped.Add(st.item, st.count, true);
    dropped.offcuts = client.inventory.offcuts;
    for (int p = 0; p < static_cast<int>(ArmourPiece::Count); p++)
        if (client.armour & Armour::Bit(static_cast<ArmourPiece>(p))) dropped.Add(Items::OfArmour(static_cast<ArmourPiece>(p)), 1, true);
    if (dropped == Inventory(Inventory::kChestSlots)) return; // (nothing to leave)
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
    client.inventory = Inventory(Inventory::kPlayerSlots);
    client.armour = 0;
    SendInventory(client);
    char where[64];
    std::snprintf(where, sizeof where, "%d, %d, %d", spot->x, spot->y, spot->z);
    SendChat(client, net::ChatMessage::Kind::Info, std::string("Your things are in a chest where you died (") + where + ")");
}

void Server::HandleUseBed(ClientState& client, const glm::ivec3& pos) {
    Block block;
    if (!InReach(client, pos, kChestReach) || !world->GetBlock(pos.x, pos.y, pos.z, block)) return;
    if (block.type == BlockType::DOOR || block.type == BlockType::DOOR_OPEN) {
        // Crouching: its hinge swaps sides; else it opens or shuts.
        if (client.flags & net::kFlagCrouching) return MirrorDoor(pos);
        return ToggleDoor(pos, block.type == BlockType::DOOR);
    }
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
    // An engineer's hammer in hand: form the structure this block is part of.
    if (client.heldItem == Items::Of(ItemType::HAMMER) && (Creative(client) || client.inventory.Count(Items::Of(ItemType::HAMMER)) > 0))
        return TryForm(&client, pos);
    // A filter pipe or a crafter: set to what's in hand.
    if (block.type == BlockType::FILTER_PIPE || block.type == BlockType::CRAFTER) {
        // (A block in hand counts as what it's worth: pebbles are stone.)
        const ItemId held = Items::IsBlock(client.heldItem) ? Items::ResourceOf(Items::BlockOf(client.heldItem))
                          : Items::IsStackable(client.heldItem) ? client.heldItem
                                                                : Items::kNone;
        Block set = block;
        if (block.type == BlockType::FILTER_PIPE) {
            set.axis = Items::FilterAxis(held);
            SendChat(client, net::ChatMessage::Kind::Info, held == Items::kNone ? "Filter: anything" : "Filter: " + Items::Id(held));
        }
        else {
            const auto& recipes = Crafting::Recipes();
            int found = -1;
            for (int i = 0; i < static_cast<int>(recipes.size()) && found < 0; i++)
                if (!recipes[i].MakesGear() && recipes[i].output == held && held != Items::kNone) found = i;
            if (held == Items::kNone) {
                const int index = static_cast<int>(block.axis) - 1;
                return SendChat(client, net::ChatMessage::Kind::Info,
                                index >= 0 && index < static_cast<int>(recipes.size()) ? std::string("Crafter: ") + recipes[index].name : "Crafter: no recipe");
            }
            if (found < 0) return SendChat(client, net::ChatMessage::Kind::Info, "Crafter can't make " + Items::Id(held));
            set.axis = static_cast<uint8_t>(found + 1);
            SendChat(client, net::ChatMessage::Kind::Info, std::string("Crafter: ") + recipes[found].name);
        }
        world->SetBlock(pos.x, pos.y, pos.z, set);
        return;
    }
    if (IsValveType(block.type)) {
        auto sig = signalBlocks.find(pos);
        if (sig != signalBlocks.end() && sig->second.config.a != 0) {
            return SendChat(client, net::ChatMessage::Kind::Info, "Valve on channel " + std::to_string(sig->second.config.a));
        }
        world->SetBlock(pos.x, pos.y, pos.z, Block(ValveSetTo(block.type, !IsOpenValve(block.type))));
        return;
    }
    if (block.type == BlockType::CAMPFIRE) {
        // Cooks one of the raw food in their hand.
        const ItemType raw = Items::ItemOf(client.heldItem);
        const ItemType cooked = raw == ItemType::RAW_VENISON ? ItemType::COOKED_VENISON : raw == ItemType::RAW_FISH ? ItemType::COOKED_FISH : ItemType::NONE;
        if (cooked == ItemType::NONE || !client.inventory.TakeUnits(raw, Block::kUnitsPerBlock)) return;
        Give(client, Items::Of(cooked), Block::kUnitsPerBlock);
        SendInventory(client);
        if (cooked == ItemType::COOKED_VENISON) Award(client, Achievement::Cook);
        return;
    }
    if (block.type != BlockType::BED) return;
    if (client.bed != pos) {
        client.bed = pos;
        client.sitesRegion = glm::ivec2(INT_MIN, INT_MIN); // (tell them, for their map)
        SendChat(client, net::ChatMessage::Kind::Info, "Respawn point set");
    }
    // At night (the sun below the horizon), they sleep; once everyone here is asleep
    // (in bed within the last few seconds), it's morning.
    if (std::sin(timeOfDay * 6.283185307179586) >= 0.0) return SendChat(client, net::ChatMessage::Kind::Info, "You can only sleep at night");
    // A horde night can't be slept through. Getting into bed before the horde would be
    // announced at dusk decides the night first, so it can't be dodged that way either.
    if (!wasNight) {
        wasNight = true;
        BeginNight();
    }
    if (creatures.Horde()) return SendChat(client, net::ChatMessage::Kind::Info, "You can't sleep with a horde coming: survive the night");
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
    // This half and the one above or below it; and a door beside it (a double door: the
    // next cell along the doorway, facing the same way or the other) likewise.
    const BlockType from = open ? BlockType::DOOR : BlockType::DOOR_OPEN, to = open ? BlockType::DOOR_OPEN : BlockType::DOOR;
    Block door;
    if (!world->GetBlock(pos.x, pos.y, pos.z, door)) return;
    std::vector<glm::ivec3> halves;
    auto addDoor = [&](const glm::ivec3& at) {
        halves.push_back(at);
        for (const int dy : { 1, -1 }) {
            Block other;
            if (world->GetBlock(at.x, at.y + dy, at.z, other) && other.type == from) {
                halves.push_back(at + glm::ivec3(0, dy, 0));
                break;
            }
        }
    };
    addDoor(pos);
    const glm::ivec3 facing = door.Facing();
    const glm::ivec3 along(facing.z, 0, facing.x); // (across the way it faces: along the doorway)
    for (const int side : { 1, -1 }) {
        const glm::ivec3 at = pos + along * side;
        Block beside;
        if (world->GetBlock(at.x, at.y, at.z, beside) && beside.type == from && std::abs(beside.Facing().x * facing.x + beside.Facing().z * facing.z) == 1) {
            addDoor(at);
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
    // Each half keeps the way it faces (where its hinge is); only open or shut changes.
    for (const auto& h : halves) {
        Block half;
        if (!world->GetBlock(h.x, h.y, h.z, half)) continue;
        half.type = to;
        world->SetBlock(h.x, h.y, h.z, half);
    }
}

void Server::MirrorDoor(const glm::ivec3& pos) {
    for (const int dy : { 0, 1, -1 }) {
        Block half;
        if (!world->GetBlock(pos.x, pos.y + dy, pos.z, half) || (half.type != BlockType::DOOR && half.type != BlockType::DOOR_OPEN)) continue;
        half.axis = static_cast<uint8_t>(half.axis ^ Block::kDoorMirrored);
        world->SetBlock(pos.x, pos.y + dy, pos.z, half);
    }
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
        const glm::vec3 eye = client.feet + glm::vec3(0.0f, Body().eyeHeight, 0.0f);
        const glm::ivec3 cell(glm::floor(eye));
        Block block;
        const bool underWater = world->GetBlock(cell.x, cell.y, cell.z, block) && UnderWater(block, eye.y - cell.y);
        air = At(eye.y, underWater, FreshAirAt(eye));
        // An oxygen tank: breathed from while anything would take your breath, filled
        // back up where the air's good.
        const bool tankWorn = (client.armour & Armour::Bit(ArmourPiece::OxygenTank)) != 0;
        client.onTank = false;
        if (tankWorn && air.cause != Cause::None && client.tank > 0.0f) {
            client.tank = std::max(0.0f, client.tank - dt);
            air = At(eye.y, false, true);
            client.onTank = true;
        }
        else if (tankWorn && air.cause == Cause::None) client.tank = std::min(kTankSeconds, client.tank + kTankRefill * dt);
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
    if (static_cast<int>(std::ceil(client.air)) != client.sentAir || client.airCause != client.sentAirCause || client.onTank != client.sentOnTank ||
        // The tank every 3 seconds' worth or so, and the moment it's full or empty (which
        // those steps would otherwise stop short of, leaving it shown just under full).
        std::abs(TankByte(client.tank) - client.sentTank) >= 8 || ((TankByte(client.tank) == 255 || TankByte(client.tank) == 0) && TankByte(client.tank) != client.sentTank))
        SendHealth(client, false);
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

void Server::HandleEat(ClientState& client, ItemId food) {
    const int value = Hunger::FoodValue(food);
    // Food you hold, while not full (creative players have it all and aren't hungry).
    if (value == 0 || Creative(client) || client.inventory.Units(food) < Block::kUnitsPerBlock || client.food >= Hunger::kMaxFood) return SendInventory(client);
    client.inventory.TakeUnits(food, Block::kUnitsPerBlock);
    client.food = std::min(Hunger::kMaxFood, client.food + value);
    Award(client, Achievement::FirstMeal);
    SendInventory(client);
    SendHealth(client, false);
}

void Server::HandleOpenChest(ClientState& client, const glm::ivec3& at, bool open) {
    client.openChest.reset();
    Block block;
    // A structure's part: its core's inventory.
    glm::ivec3 pos = at;
    if (open && world->GetBlock(at.x, at.y, at.z, block) && block.type == BlockType::MULTIBLOCK_PART && InReach(client, at, kChestReach)) {
        glm::ivec3 core;
        if (CoreOf(at, core)) pos = core;
    }
    if (open && (pos != at || InReach(client, pos, kChestReach)) && world->GetBlock(pos.x, pos.y, pos.z, block) && IsStorageType(block.type)) {
        client.openChest = pos;
        const bool unopened = chests.count(pos) == 0;
        const Inventory& chest = ChestAt(pos);
        // The first look into a dungeon's chest.
        if (unopened && !chest.Empty()) Award(client, Achievement::TreasureHunter);
    }
    SendChest(client);
}

Inventory* Server::OpenChestOf(ClientState& client) {
    if (!client.openChest) return nullptr;
    const glm::ivec3 pos = *client.openChest;
    Block block;
    if (!world->GetBlock(pos.x, pos.y, pos.z, block) || !IsStorageType(block.type)) return nullptr;
    // (A structure's part is opened as its core, which may be further away.)
    if (!InReach(client, pos, kChestReach + 2.0f) && !IsMachineInventory(block.type)) return nullptr;
    return &ChestAt(pos);
}

void Server::HandleChestMove(ClientState& client, const glm::ivec3& pos, ItemId item, uint32_t count, bool intoChest) {
    // Only with that chest open, still there and in reach.
    Inventory* chest = client.openChest == pos ? OpenChestOf(client) : nullptr;
    if (!chest) return SendInventory(client);
    Inventory& from = intoChest ? client.inventory : *chest;
    Inventory& to = intoChest ? *chest : client.inventory;
    const uint32_t n = std::min({ count, from.Count(item), to.Room(item) });
    from.Take(item, n);
    to.Add(item, n);
    client.inventory.Trim(Inventory::kPlayerSlots);
    chest->Trim(Inventory::kChestSlots);
    SendInventory(client);
    SendChestToViewers(pos);
}

void Server::SendChest(ClientState& client) {
    net::ChestContents m;
    m.open = client.openChest.has_value();
    if (client.openChest) {
        m.pos = *client.openChest;
        m.items = ChestAt(*client.openChest);
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
    Inventory& chest = chests[pos];
    chest = Inventory(Inventory::kChestSlots);
    for (const auto& [type, units] : Dungeons::LootFor(terrain, pos)) chest.AddUnits(type, units, true);
    for (const auto& [type, units] : Dwarves::LootFor(terrain, pos)) chest.AddUnits(type, units, true);
    return chest;
}

void Server::RemoveChest(const glm::ivec3& pos, ClientState* breaker) {
    if (breaker) ChestAt(pos); // (an unopened dungeon chest still holds its loot)
    auto it = chests.find(pos);
    if (it == chests.end()) return;
    // What was in it spills out where it was.
    if (breaker) {
        const Inventory& held = it->second;
        for (const Stack& st : held.slots)
            if (!st.Empty()) dropped.DropItems(st.item, st.count, pos);
        for (size_t t = 0; t < held.offcuts.size(); t++)
            if (held.offcuts[t]) dropped.DropUnits(Items::OfKind(t), held.offcuts[t], pos);
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
    // A crossbow in hand (theirs), or a bow of their own; an arrow to shoot (survival),
    // and not too often.
    const bool crossbow = client.heldItem == Items::Of(ItemType::CROSSBOW) && (Creative(client) || client.inventory.Count(Items::Of(ItemType::CROSSBOW)) > 0);
    if (now < client.nextShot || (!crossbow && Usable(client, Tool::Bow) != Tool::Bow)) return;
    if (!Creative(client)) {
        if (!client.inventory.TakeUnits(ItemType::ARROW, Block::kUnitsPerBlock)) return SendInventory(client);
        SendInventory(client);
    }
    client.nextShot = now + (crossbow ? kCrossbowInterval : kShotInterval) * 0.8; // (a little slack for timing)
    const glm::vec3 dir = glm::normalize(direction);
    const glm::vec3 eye = client.feet + glm::vec3(0.0f, (client.flags & net::kFlagCrouching) ? Body().crouchEyeHeight : Body().eyeHeight, 0.0f);
    arrows.push_back({ eye + dir * 0.4f, dir * (crossbow ? kBoltSpeed : kArrowSpeed), client.id, now, -1.0, crossbow ? kBoltDamage : kArrowDamage });
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
                if (!world->GetBlock(p.x, p.y, p.z, b) || b.IsAir() || b.type == BlockType::OBSIDIAN || b.type == BlockType::REINFORCED_CONCRETE) continue; // (obsidian and reinforced concrete take it)
                if (b.type == BlockType::TNT && p != pos) {
                    LightTnt(p, 0.4 + 0.1 * (std::abs(x) + std::abs(z)));
                    continue;
                }
                if (IsStorageType(b.type)) RemoveChest(p, nullptr);
                if (b.type == BlockType::SIGN && signs.erase(p)) BroadcastSign(p);
                world->SetBlock(p.x, p.y, p.z, Block());
            }
    // It leaves fires round it, where there's something to burn.
    Scorch(pos, radius + 1, 0.15f);
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
        client.knockedUntil = Now() + kKnockSeconds;
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
        Give(client, ItemType::RAW_FISH, Block::kUnitsPerBlock);
        SendInventory(client);
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
                creatures.Damage(id, a.damage, a.position - a.velocity * 0.05f, now, killed);
                if (!killed && a.shooter != kCreatureShooter) creatures.Provoke(id, a.shooter, now);
                if (killed)
                    for (auto& [peer, shooter] : clients)
                        if (shooter.id == a.shooter) {
                            if (type == CreatureType::DwarfKing) KingSlain(shooter);
                            if (type == CreatureType::ForgeGolem) GolemSlain(shooter);
                            Award(shooter, Achievement::Marksman);
                            if (type == CreatureType::Deer) Award(shooter, Achievement::Hunter);
                            else if (SpecOf(type).hostile) Award(shooter, Achievement::DwarfSlayer);
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
                    std::string by = a.shooter == kCreatureShooter ? "a dwarf crossbowman" : "someone";
                    for (const auto& [p2, s] : clients)
                        if (s.id == a.shooter) by = s.name;
                    const double before = target.invulnerableUntil;
                    Hurt(target, a.damage, "was shot by " + by);
                    if (target.health > 0 && target.lastHurt >= now && target.invulnerableUntil == before) {
                        const glm::vec2 push = glm::length(glm::vec2(a.velocity.x, a.velocity.z)) > 1e-3f ? glm::normalize(glm::vec2(a.velocity.x, a.velocity.z)) : glm::vec2(0.0f);
                        net.Send(target.peer, net::kChannelReliable, net::Encode(net::Knockback{ glm::vec3(push.x * 5.0f, 4.0f, push.y * 5.0f) }));
                        target.knockedUntil = now + kKnockSeconds;
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

void Server::HandleDropItem(ClientState& client, const net::DropItem& m) {
    if (client.health <= 0 || !client.hasState) return;
    // The armour slots as a little inventory of their own (as in HandleInventoryMove).
    Inventory armourBox = ArmourBox(client.armour);
    const net::Box boxKind = static_cast<net::Box>(m.box);
    Inventory* box = boxKind == net::Box::Player ? &client.inventory : boxKind == net::Box::Armour ? &armourBox : OpenChestOf(client);
    if (!box || m.slot >= box->slots.size() || box->slots[m.slot].Empty()) return SendInventory(client), SendChest(client);
    Stack& st = box->slots[m.slot];
    const uint32_t n = m.count == 0 ? st.count : std::min<uint32_t>(m.count, st.count);
    const ItemId item = st.item;
    st.count = static_cast<uint8_t>(st.count - n);
    if (st.count == 0) st = {};
    if (boxKind == net::Box::Armour) client.armour = WornFrom(armourBox);
    // Thrown from the eye, the way they're looking, a little up: as hard as they held
    // the throw for.
    const glm::vec3 dir = glm::length(m.direction) > 1e-3f ? glm::normalize(m.direction) : glm::vec3(0.0f, 0.0f, -1.0f);
    const bool crouching = (client.flags & net::kFlagCrouching) != 0;
    const glm::vec3 eye = client.feet + glm::vec3(0.0f, (crouching ? Body().crouchHeight : Body().height) - 0.2f, 0.0f);
    const float speed = net::kThrowSpeed + (net::kThrowSpeedMax - net::kThrowSpeed) * (m.strength / 255.0f);
    dropped.Drop(item, n, 0, eye + dir * 0.3f - glm::vec3(0.0f, 0.3f, 0.0f), dir * speed + glm::vec3(0.0f, 1.5f, 0.0f), DroppedItems::kThrowDelay);
    SendInventory(client);
    if (boxKind == net::Box::Chest) SendChestToViewers(*client.openChest);
}

void Server::UpdateDroppedItems() {
    constexpr float dt = 1.0f / kTickRate;
    // Plants and levers knocked off what held them drop where they were.
    {
        std::vector<std::pair<glm::ivec3, Block>> fallen;
        world->TakeKnockedOff(fallen);
        for (const auto& [pos, lever] : fallen) dropped.DropUnits(Items::ResourceOf(lever.type), static_cast<uint32_t>(lever.ResourceUnits()), pos);
    }
    // Who could take what: in the world, alive, and (flying through blocks) not in no-clip flight.
    struct Taker {
        ClientState* client;
        Collision::AABB body;
        glm::vec3 chest;
    };
    std::vector<Taker> takers;
    for (auto& [peer, client] : clients) {
        if (!client.welcomed || !client.hasState || client.health <= 0) continue;
        const Collision::AABB body = BodyAt(client.feet, 0.0f, (client.flags & net::kFlagCrouching) != 0);
        takers.push_back({ &client, body, (body.min + body.max) * 0.5f });
    }
    // Whether any of it would fit.
    auto room = [](const Inventory& inv, const DroppedItems::Item& d) {
        if (d.count > 0) return inv.Room(d.item) > 0;
        return inv.FitsUnits(d.item, d.units);
    };
    for (auto& d : dropped.All()) {
        if (d.age < d.pickupDelay) continue;
        // What a block left flies to whoever broke it, if they're near and have room.
        if (d.owner && d.age < d.ownerFor)
            for (const Taker& t : takers)
                if (t.client->id == d.owner && glm::distance(d.position, t.chest) < 8.0f && room(t.client->inventory, d)) DroppedItems::FlyTo(d, t.chest, dt);
        // Anything else close by drifts in to whoever's nearest.
        if (!d.flying) {
            const Taker* nearest = nullptr;
            float best = 1.6f;
            for (const Taker& t : takers) {
                const glm::vec3 c = glm::clamp(d.position, t.body.min, t.body.max);
                const float dist = glm::distance(c, d.position);
                if (dist < best && room(t.client->inventory, d)) best = dist, nearest = &t;
            }
            if (nearest) DroppedItems::FlyTo(d, nearest->chest, dt);
        }
    }
    dropped.Update(*world, dt);
    // Picked up: touching them (into their inventory, as much as fits).
    std::vector<uint32_t> taken;
    for (auto& d : dropped.All()) {
        if (d.age < d.pickupDelay) continue;
        for (const Taker& t : takers) {
            const Collision::AABB grab{ t.body.min - glm::vec3(0.3f), t.body.max + glm::vec3(0.3f) };
            if (glm::any(glm::lessThan(d.position, grab.min)) || glm::any(glm::greaterThan(d.position, grab.max))) continue;
            Inventory& inv = t.client->inventory;
            bool changed = false;
            if (d.count > 0) {
                const uint32_t left = inv.Add(d.item, d.count);
                changed = left < d.count;
                d.count = static_cast<uint8_t>(left);
            }
            if (d.units > 0 && d.count == 0 && inv.FitsUnits(d.item, d.units)) {
                inv.AddUnits(d.item, d.units);
                d.units = 0, changed = true;
            }
            if (changed) SendInventory(*t.client);
            if (d.count == 0 && d.units == 0) {
                taken.push_back(d.id);
                break;
            }
        }
    }
    for (const uint32_t id : taken) dropped.Remove(id);
}

void Server::SendDroppedItems() {
    constexpr float kSendRange = 48.0f;
    for (auto& [peer, client] : clients) {
        if (!client.welcomed) continue;
        net::ItemsNear m;
        bool moving = false;
        uint64_t hash = 0;
        for (const auto& d : dropped.All()) {
            if (glm::distance(d.position, client.feet) >= kSendRange) continue;
            m.items.push_back({ d.id, d.item, d.count, d.position, d.velocity });
            moving |= d.flying || !d.onGround || glm::length(d.velocity) > 1e-3f;
            hash = (hash ^ (static_cast<uint64_t>(d.id) * 0x9E3779B97F4A7C15ull + d.count)) * 0x100000001B3ull;
        }
        if (m.items.size() > net::kMaxItemsSent) {
            // The nearest.
            std::partial_sort(m.items.begin(), m.items.begin() + net::kMaxItemsSent, m.items.end(), [&](const net::ItemInfo& a, const net::ItemInfo& b) {
                return glm::distance(a.position, client.feet) < glm::distance(b.position, client.feet);
            });
            m.items.resize(net::kMaxItemsSent);
        }
        // Every tick while anything's moving or it's changed; now and then otherwise (it
        // goes unreliably).
        client.itemsQuiet++;
        if (!moving && hash == client.sentItems && client.itemsQuiet < kTickRate) continue;
        if (m.items.empty() && client.sentItems == 0) continue; // (nothing, and they know it)
        client.sentItems = m.items.empty() ? 0 : (hash ? hash : 1);
        client.itemsQuiet = 0;
        net.Send(peer, net::kChannelUnreliable, net::Encode(m));
    }
}

void Server::LoosenLeaves(const std::vector<glm::ivec3>& logsGone) {
    const double now = Now();
    for (const auto& leaf : Trees::LooseLeaves(*world, logsGone, &placedLeaves)) {
        if (leafFalls.count(leaf)) continue;
        const float r = Noise::Random01(leaf.x, leaf.y, leaf.z, static_cast<uint32_t>(tickCount));
        leafFalls[leaf] = now + Trees::kLeafFallMin + (Trees::kLeafFallMax - Trees::kLeafFallMin) * r;
    }
}

void Server::UpdateLeaves() {
    if (leafFalls.empty()) return;
    const double now = Now();
    std::vector<glm::ivec3> due;
    for (const auto& [pos, when] : leafFalls)
        if (now >= when) due.push_back(pos);
    for (const auto& pos : due) {
        leafFalls.erase(pos);
        Block b;
        // (Still leaves, and still nothing holding them up: a log may have been put back.)
        if (!world->GetBlock(pos.x, pos.y, pos.z, b) || !IsLeavesType(b.type) || Trees::LogNear(*world, pos) || placedLeaves.count(pos)) continue;
        world->SetBlock(pos.x, pos.y, pos.z, b.HasWater() ? Block::Water(b.WaterVolume()) : Block());
        if (Noise::Random01(pos.x, pos.y, pos.z, 0x5A7u) < Trees::kSaplingChance)
            dropped.DropUnits(b.type == BlockType::PINE_LEAVES ? BlockType::PINE_SAPLING : BlockType::SAPLING, Block::kUnitsPerBlock, pos);
    }
}

bool Server::Ignite(const glm::ivec3& pos) {
    Block b;
    if (!world->GetBlock(pos.x, pos.y, pos.z, b) || !Fire::CanBurnIn(b) || world->GetFireCount() >= Fire::kMaxFires) return false;
    world->SetBlock(pos.x, pos.y, pos.z, Block(BlockType::FIRE));
    fireLit[pos] = Now();
    return true;
}

void Server::Scorch(const glm::ivec3& pos, int radius, float chance) {
    for (int dx = -radius; dx <= radius; dx++)
        for (int dy = -radius; dy <= radius; dy++)
            for (int dz = -radius; dz <= radius; dz++) {
                const glm::ivec3 c = pos + glm::ivec3(dx, dy, dz);
                Block b;
                if (!world->GetBlock(c.x, c.y, c.z, b) || !Fire::CanBurnIn(b)) continue;
                bool fuel = false;
                for (int f = 0; f < kFaceCount && !fuel; f++) {
                    Block n;
                    fuel = world->GetBlock(c.x + kFaceOffsets[f][0], c.y + kFaceOffsets[f][1], c.z + kFaceOffsets[f][2], n) && Fire::Burns(n.type);
                }
                if (fuel && FireRandom() < chance) Ignite(c);
            }
}

void Server::UpdateFire() {
    const double now = Now();
    if (now < nextFire) return;
    nextFire = now + Fire::kInterval;
    const float dt = static_cast<float>(Fire::kInterval);
    std::vector<glm::ivec3> fires;
    world->GetFires(fires);
    // Items lying in fire or lava are lost.
    std::vector<uint32_t> burnt;
    for (const auto& d : dropped.All()) {
        const glm::ivec3 c(glm::floor(d.position));
        Block b;
        if (world->GetBlock(c.x, c.y, c.z, b) && (b.type == BlockType::FIRE || b.IsLava())) burnt.push_back(d.id);
    }
    for (const uint32_t id : burnt) dropped.Remove(id);
    if (fires.empty()) {
        fireLit.clear();
        return;
    }
    // Creatures in it are hurt.
    {
        std::vector<std::pair<uint16_t, glm::vec3>> hurt;
        for (const auto& c : creatures.All())
            for (const int dy : { 0, 1 }) {
                const glm::ivec3 cell(glm::floor(c.feet + glm::vec3(0.0f, 0.2f + dy, 0.0f)));
                Block b;
                if (world->GetBlock(cell.x, cell.y, cell.z, b) && b.type == BlockType::FIRE) {
                    hurt.emplace_back(c.id, c.feet);
                    break;
                }
            }
        for (const auto& [id, feet] : hurt) {
            bool killed = false;
            creatures.Damage(id, 1, feet - glm::vec3(0.0f, 0.5f, 0.0f), now, killed); // (a little hop up out of it)
        }
    }
    static const glm::ivec3 kTries[] = { { 0, 1, 0 }, { 0, 2, 0 }, { 1, 1, 0 }, { -1, 1, 0 }, { 0, 1, 1 }, { 0, 1, -1 }, { 1, 0, 0 }, { -1, 0, 0 },
                                         { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 1 }, { -1, 0, -1 }, { 1, 0, -1 }, { -1, 0, 1 }, { 0, -1, 0 }, { 1, -1, 0 } };
    for (const auto& f : fires) {
        Block self;
        if (!world->GetBlock(f.x, f.y, f.z, self) || self.type != BlockType::FIRE) continue;
        if (!fireLit.count(f)) fireLit[f] = now;
        // Rain on it (open to the sky): it spreads less, and now and then goes out.
        bool rainedOn = false;
        if (raining) {
            rainedOn = true;
            for (int y = f.y + 1; y <= f.y + 40 && rainedOn; y++) {
                Block b;
                if (world->GetBlock(f.x, y, f.z, b) && !b.IsAir() && !b.IsPlant()) rainedOn = false;
            }
        }
        if (rainedOn && FireRandom() < 0.15f * dt) {
            world->SetBlock(f.x, f.y, f.z, Block());
            fireLit.erase(f);
            continue;
        }
        // It eats what it touches (and often takes its place), and sets off TNT.
        bool fuel = false;
        for (int i = 0; i < kFaceCount; i++) {
            const glm::ivec3 n = f + glm::ivec3(kFaceOffsets[i][0], kFaceOffsets[i][1], kFaceOffsets[i][2]);
            Block b;
            if (!world->GetBlock(n.x, n.y, n.z, b)) continue;
            if (b.type == BlockType::TNT) LightTnt(n, 0.5 + FireRandom());
            if (!Fire::Burns(b.type)) continue;
            fuel = true;
            if (FireRandom() >= Fire::BurnRate(b.type) * dt * (rainedOn ? 0.5f : 1.0f)) continue;
            if (IsStorageType(b.type)) RemoveChest(n, nullptr); // (what was in it burns too)
            if (b.type == BlockType::SIGN && signs.erase(n)) BroadcastSign(n);
            if (b.type == BlockType::CROP || b.type == BlockType::SAPLING || b.type == BlockType::PINE_SAPLING) crops.erase(n);
            world->SetBlock(n.x, n.y, n.z, Block());
            if (FireRandom() < 0.7f) Ignite(n);
        }
        // With nothing left to burn, it dies down.
        if (!fuel) {
            const double lit = fireLit[f];
            const float hold = Fire::kDieOut * (1.0f + Noise::Random01(f.x, f.y, f.z, 0xF1Eu));
            if (now - lit > hold) {
                world->SetBlock(f.x, f.y, f.z, Block());
                fireLit.erase(f);
            }
            continue;
        }
        // It spreads: open cells round it (above it most) beside something that burns.
        float tries = Fire::kSpreadTries * dt;
        while (tries > 0.0f) {
            if (tries < 1.0f && FireRandom() >= tries) break;
            tries -= 1.0f;
            const glm::ivec3 c = f + kTries[static_cast<size_t>(FireRandom() * std::size(kTries)) % std::size(kTries)];
            Block b;
            if (!world->GetBlock(c.x, c.y, c.z, b) || !Fire::CanBurnIn(b)) continue;
            float best = 0.0f;
            for (int i = 0; i < kFaceCount; i++) {
                Block n;
                if (world->GetBlock(c.x + kFaceOffsets[i][0], c.y + kFaceOffsets[i][1], c.z + kFaceOffsets[i][2], n)) best = std::max(best, Fire::Flammability(n.type));
            }
            const float chance = best * (c.y > f.y ? 1.0f : 0.6f) * (rainedOn ? Fire::kRainDamping : 1.0f);
            if (FireRandom() < chance) Ignite(c);
        }
    }
    // (Forget fires that are gone.)
    if (fireLit.size() > fires.size() * 2 + 64)
        for (auto it = fireLit.begin(); it != fireLit.end();) {
            Block b;
            if (!world->GetBlock(it->first.x, it->first.y, it->first.z, b) || b.type != BlockType::FIRE) it = fireLit.erase(it);
            else ++it;
        }
}

void Server::HandleUseBucket(ClientState& client, const net::UseBucket& m) {
    const glm::ivec3& p = m.pos;
    Block b;
    if (!InReach(client, p, EditReach(client)) || !world->GetBlock(p.x, p.y, p.z, b)) return SendInventory(client);
    const bool creative = Creative(client);
    const ItemId bucket = Items::Of(ItemType::BUCKET), full = Items::Of(ItemType::WATER_BUCKET);
    if (!m.pour) {
        // Scooping up a block of water: only from a full cell (a bucket holds a block).
        if (!b.IsFullWater() || (!creative && client.inventory.Count(bucket) == 0)) return SendInventory(client);
        world->SetBlock(p.x, p.y, p.z, Block());
        if (!creative) {
            client.inventory.Take(bucket, 1);
            if (client.inventory.Add(full, 1) != 0)
                dropped.Drop(full, 1, 0, client.feet + glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f), DroppedItems::kBlockDelay);
        }
        return SendInventory(client);
    }
    // Pouring it out: a block of water there (fire and plants washed away), and fires round
    // it put out by the splash.
    if ((!creative && client.inventory.Count(full) == 0) || !(b.IsReplaceableByWater() || b.type == BlockType::FIRE) || b.HasWater()) return SendInventory(client);
    world->SetBlock(p.x, p.y, p.z, Block::Water(Block::kMaxWaterVolume));
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                Block f;
                const glm::ivec3 c = p + glm::ivec3(dx, dy, dz);
                if (world->GetBlock(c.x, c.y, c.z, f) && f.type == BlockType::FIRE) world->SetBlock(c.x, c.y, c.z, Block());
            }
    if (glm::distance(client.feet, glm::vec3(p) + glm::vec3(0.5f)) < 2.5f) client.burningUntil = 0.0; // (splashed: out)
    if (!creative) {
        client.inventory.Take(full, 1);
        client.inventory.Add(bucket, 1);
    }
    SendInventory(client);
}

void Server::UpdateLava() {
    const double now = Now();
    if (now < nextLava) return;
    nextLava = now + Lava::kInterval;
    std::vector<glm::ivec3> woken;
    world->TakeLavaWake(woken);
    if (woken.empty()) return;
    std::vector<Lava::Change> changes;
    std::vector<Lava::Boil> boils;
    Lava::Step(*world, woken, changes, boils);
    for (const auto& c : changes) world->SetBlock(c.pos.x, c.pos.y, c.pos.z, c.block);
    // Lava sets fire to what burns beside it.
    for (const auto& p : woken) {
        Block b;
        if (world->GetBlock(p.x, p.y, p.z, b) && b.IsLava()) Scorch(p, 1, 0.3f);
    }
    // Water touching lava boils away into a cloud of steam, with a hiss.
    for (const auto& b : boils) steam.Add(b.pos, b.steam);
}

void Server::UpdateSteam() {
    const double now = Now();
    if (now < nextSteam) return;
    nextSteam = now + SteamCloud::kInterval;
    steam.Update(*world);
}

void Server::SendSteam() {
    const double now = Now();
    if (now < nextSteamSend) return;
    nextSteamSend = now + 0.2;
    constexpr int kRange = 64;
    for (auto& [peer, client] : clients) {
        if (!client.welcomed) continue;
        net::SteamNear m;
        m.base = glm::ivec3(glm::floor(client.feet));
        for (const auto& [cell, units] : steam.All()) {
            const glm::ivec3 d = cell - m.base;
            if (std::abs(d.x) > kRange || std::abs(d.y) > kRange || std::abs(d.z) > kRange) continue;
            m.cells.push_back({ static_cast<int16_t>(d.x), static_cast<int16_t>(d.y), static_cast<int16_t>(d.z), static_cast<uint8_t>(std::min<int>(255, units)) });
        }
        if (m.cells.size() > net::kMaxSteamSent) {
            // The nearest.
            auto far = [](const net::SteamCell& c) { return c.dx * c.dx + c.dy * c.dy + c.dz * c.dz; };
            std::nth_element(m.cells.begin(), m.cells.begin() + net::kMaxSteamSent, m.cells.end(),
                             [&](const net::SteamCell& a, const net::SteamCell& b) { return far(a) < far(b); });
            m.cells.resize(net::kMaxSteamSent);
        }
        if (m.cells.empty() && !client.sentSteam) continue; // (none, and they know it)
        client.sentSteam = !m.cells.empty();
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
        bool spiked = false, burning = false, scorched = false;
        for (int i = 0; i < 4; i++) {
            Block b;
            const glm::ivec3& f = feetCells[i];
            if (world->GetBlock(f.x, f.y, f.z, b)) {
                if (b.type == BlockType::SPIKES) spiked = true;
                if (b.type == BlockType::CAMPFIRE) burning = true;
                if (b.type == BlockType::PRESSURE_PLATE) {
                    const bool fresh = !plateSignal.count(f);
                    PressPlate(f, now);
                }
            }
            const glm::ivec3& u = underCells[i];
            if (world->GetBlock(u.x, u.y, u.z, b) && b.type == BlockType::CRUMBLING_BRICK && !crumbling.count(u)) crumbling[u] = now + kCrumbleDelay;
            if (world->GetBlock(u.x, u.y, u.z, b) && b.type == BlockType::MAGMA) scorched = true;
        }
        // Wading in lava burns, hard.
        {
            const Collision::AABB body = BodyAt(client.feet, 0.05f, (client.flags & net::kFlagCrouching) != 0);
            bool inLava = false, inFire = false, inWater = false;
            int steamIn = 0; // the thickest steam the body's in
            for (int x = static_cast<int>(std::floor(body.min.x)); x <= static_cast<int>(std::floor(body.max.x)); x++)
                for (int y = static_cast<int>(std::floor(body.min.y)); y <= static_cast<int>(std::floor(body.max.y)); y++)
                    for (int z = static_cast<int>(std::floor(body.min.z)); z <= static_cast<int>(std::floor(body.max.z)); z++) {
                        Block b;
                        if (!world->GetBlock(x, y, z, b)) continue;
                        inLava |= b.IsLava();
                        inFire |= b.type == BlockType::FIRE;
                        inWater |= b.HasWater();
                        steamIn = std::max(steamIn, steam.At({ x, y, z }));
                    }
            // Fire (and lava) set you alight, and you go on burning a while after you're
            // out of it, unless you get into water.
            if ((inFire || inLava) && !Creative(client)) client.burningUntil = now + Fire::kBurnSeconds;
            if (inWater) client.burningUntil = 0.0;
            if ((now < client.burningUntil) != client.sentBurning) SendHealth(client, false); // (alight, or out: their screen shows it)
            if (now < client.burningUntil && now >= client.nextBurnHurt) {
                client.nextBurnHurt = now + Fire::kBurnHurtInterval;
                Hurt(client, Fire::kBurnDamage, inFire ? "was burned to death" : "burned to death", false);
            }
            // Thick steam scalds (armour doesn't keep it out).
            if (steamIn >= SteamCloud::kScaldUnits && now >= client.nextScald && !(client.armour & Armour::Bit(ArmourPiece::Mask))) { // (a mask keeps it out)
                client.nextScald = now + SteamCloud::kScaldInterval;
                Hurt(client, SteamCloud::kScaldDamage, "was scalded by steam", false);
            }
            if (inLava && now >= client.nextLavaBurn) {
                client.nextLavaBurn = now + 0.5;
                Hurt(client, 3, "tried to swim in lava");
            }
        }
        if (scorched && now >= client.nextSpikeHit) {
            client.nextSpikeHit = now + 0.75;
            Hurt(client, 1, "was scorched by magma");
        }
        if (client.feet.y < TerrainGenerator::kDeepTop) Award(client, Achievement::DeepDelver);
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
    bool night = sun < -0.12f;
    // Someone got into bed at dusk, before that: the night has already
    // begun, and lasts on through the rest of the evening (see HandleUseBed).
    if (!night && wasNight && sun < 0.0f && std::cos(timeOfDay * 6.283185307179586) < 0.0) night = true;
    if (night == wasNight) return;
    wasNight = night;
    if (night) BeginNight();
    else if (creatures.Horde()) SetHorde(false);
    if (!night) creatures.EndRaids();
}

void Server::BeginNight() {
    // The world's first night is a quiet one: no horde, no raids (time to build).
    nights++;
    if (nights <= 1) return;
    if (creatures.NaturalSpawning()) {
        weatherSeed = weatherSeed * 1664525u + 1013904223u;
        if ((weatherSeed >> 8) % static_cast<uint32_t>(std::max(1, options.hordeOdds)) == 0) SetHorde(true);
        // Not a horde: now and then a raid on a survival player at home (by their bed).
        else
            for (auto& [peer, client] : clients) {
                weatherSeed = weatherSeed * 1664525u + 1013904223u;
                if (!client.welcomed || Creative(client) || !client.bed || (weatherSeed >> 8) % kRaidOdds != 0) continue;
                if (glm::distance(glm::vec3(*client.bed), client.feet) > 48.0f) continue;
                StartRaid(client);
            }
    }
}

bool Server::StartRaid(ClientState& client) {
    if (!client.bed) return false;
    const int came = creatures.StartRaid(*world, glm::vec3(*client.bed) + glm::vec3(0.5f, 0.0f, 0.5f));
    if (came == 0) return false;
    SendChat(client, net::ChatMessage::Kind::Info, "War drums in the dark... a dwarf raid is coming for your home!");
    Log(client.name + " is raided (" + std::to_string(came) + " dwarves)");
    return true;
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
    UpdateDroppedItems();
    UpdateLava();
    UpdateSteam();
    UpdateLeaves();
    UpdateFire();
    UpdateFishing();
    UpdateTnt();
    section(2);
    UpdatePower();
    UpdateMiningPosts();
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
    SendDroppedItems();
    SendSteam();
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
    // (Dropping chunks goes out even while the link is busy: after a teleport, the old
    // place must be forgotten at once, not once the new one has streamed in.)
    if (net.GetStats(client.peer).queuedReliableBytes > kMaxQueuedBytes) return; // let the link catch up

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
    snapshot.timeOfDay = TimeOfDayFloat();
    snapshot.rain = static_cast<uint8_t>(std::lround(rainLevel * 255.0f));
    snapshot.storm = storm ? 1 : 0;
    for (const auto& [peer, client] : clients) {
        if (!client.welcomed || !client.hasState) continue;
        // Flashes red for others while just hurt.
        const uint8_t flags = static_cast<uint8_t>((client.flags & ~(net::kFlagHurt | net::kFlagBurning)) | (Now() - client.lastHurt < 0.3 ? net::kFlagHurt : 0) |
                                                   (Now() < client.burningUntil ? net::kFlagBurning : 0));
        // (Armour shows only in survival, where it's worn.)
        snapshot.players.push_back({ client.id, client.feet, client.yaw, client.pitch, flags, client.heldTool, client.heldItem,
                                     static_cast<uint8_t>(Creative(client) ? 0 : client.armour) });
    }
    const auto packet = net::Encode(snapshot);
    for (const auto& [peer, client] : clients)
        if (client.welcomed) net.Send(peer, net::kChannelUnreliable, packet);
}
