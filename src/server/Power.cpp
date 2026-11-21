// Air power: water wheels, air pipes and machines (see world/Power.h).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include "server/Server.h"
#include "server/ServerWorld.h"

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

const glm::ivec3 kFaces[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

uint8_t ToByte(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

// What a drill won't dig through: things people keep, other machinery, and liquids.
bool StopsDrill(const Block& b) {
    if (b.IsLiquid() || b.HasWater()) return true;
    switch (b.type) {
    case BlockType::CHEST:
    case BlockType::BED:
    case BlockType::DOOR:
    case BlockType::DOOR_OPEN:
    case BlockType::SIGN:
    case BlockType::TNT:
    case BlockType::TNT_LIT:
    case BlockType::PUMP:
    case BlockType::PIPE:
    case BlockType::SLUICE: return true;
    default: return Power::IsPart(b.type);
    }
}

} // namespace

void Server::UpdatePower() {
    const double now = Now();
    if (now < nextPowerUpdate) return;
    nextPowerUpdate = now + Power::kInterval;

    world->TakePower(machineList, wheelFlow);
    auto typeAt = [&](const glm::ivec3& p) {
        Block b;
        return world->GetBlock(p.x, p.y, p.z, b) ? b.type : BlockType::COUNT;
    };

    // Wheels: how fast the water past them is going (each unit that moves is counted
    // leaving one cell and arriving in another: half of it is flow).
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> present(machineList.begin(), machineList.end());
    for (auto it = wheels.begin(); it != wheels.end();) it = present.count(it->first) ? std::next(it) : wheels.erase(it);
    for (auto it = drills.begin(); it != drills.end();) it = present.count(it->first) ? std::next(it) : drills.erase(it);
    for (const auto& [pos, moved] : wheelFlow) {
        Wheel& w = wheels[pos];
        const float reading = moved * 0.5f / static_cast<float>(Power::kInterval);
        w.flow += (reading - w.flow) * Power::kWheelSmoothing;
        if (w.flow < 0.5f) w.flow = 0.0f;
        w.speed = Power::WheelSpeed(w.flow);
    }

    // Networks: everything joined to each wheel or machine.
    std::unordered_map<glm::ivec3, float, Coords::IVec3Hash> newAir;
    networks.clear();
    networkOf.clear();
    std::vector<glm::ivec3> members;
    for (const auto& start : machineList) {
        if (networkOf.count(start)) continue;
        const int index = static_cast<int>(networks.size());
        networks.emplace_back();
        AirNetwork& net = networks.back();
        members.assign(1, start);
        networkOf[start] = index;
        for (size_t i = 0; i < members.size() && members.size() < static_cast<size_t>(Power::kMaxNetwork); i++)
            for (const auto& d : kFaces) {
                const glm::ivec3 n = members[i] + d;
                if (networkOf.count(n) || !Power::IsPart(typeAt(n))) continue;
                networkOf[n] = index;
                members.push_back(n);
            }

        std::vector<std::pair<glm::ivec3, Drill*>> netDrills;
        for (const auto& m : members) {
            auto a = air.find(m);
            if (a != air.end()) net.air += a->second;
            const BlockType t = typeAt(m);
            if (t == BlockType::AIR_PIPE) net.pipes++;
            else if (t == BlockType::WATER_WHEEL) {
                net.wheels++;
                const float speed = wheels.count(m) ? wheels[m].speed : 0.0f;
                if (speed > 0.0f) net.turning++;
                net.supply += speed * Power::kWheelMaxAir;
            }
            else if (t == BlockType::DRILL) {
                net.drills++;
                netDrills.emplace_back(m, &drills[m]);
            }
        }
        net.capacity = static_cast<float>(members.size()) * Power::kAirPerCell;
        net.air = std::min(net.capacity, net.air + net.supply * static_cast<float>(Power::kInterval));

        // Machines take their turn: whichever has waited longest goes first.
        std::sort(netDrills.begin(), netDrills.end(), [](const auto& a, const auto& b) { return a.second->nextDig < b.second->nextDig; });
        for (auto& [pos, drill] : netDrills) {
            if (now >= drill->nextDig) {
                if (net.Pressure() < Power::kMinPressure || net.air < Power::kDrillAir) drill->state = Power::DrillState::NoPressure;
                else if (DrillDig(pos, *drill)) {
                    net.air -= Power::kDrillAir;
                    drill->nextDig = now + Power::kDrillInterval;
                    drill->lastDug = now;
                }
            }
            if (now - drill->lastDug < Power::kDrillInterval * 1.5) drill->state = Power::DrillState::Working;
            if (drill->state == Power::DrillState::Working) net.working++;
        }

        const float share = net.air / static_cast<float>(members.size());
        for (const auto& m : members) newAir[m] = share;
    }
    air.swap(newAir); // (parts in no network lose their air)
    SendMachines();
}

bool Server::DrillDig(const glm::ivec3& pos, Drill& drill) {
    // Somewhere to put what it digs: a chest touching it (not under it, where it digs).
    std::optional<glm::ivec3> chestAt;
    for (const auto& d : kFaces) {
        Block b;
        if (d.y < 0 || !world->GetBlock(pos.x + d.x, pos.y + d.y, pos.z + d.z, b) || b.type != BlockType::CHEST) continue;
        chestAt = pos + d;
        break;
    }
    if (!chestAt) {
        drill.state = Power::DrillState::NoChest;
        return false;
    }
    // The first thing under it, down the shaft it has dug (from the top each time, so
    // a block dropped back in is dug out again).
    for (int depth = 1; depth <= Power::kDrillDepth; depth++) {
        const glm::ivec3 p = pos - glm::ivec3(0, depth, 0);
        Block b;
        if (!world->GetBlock(p.x, p.y, p.z, b)) break; // not loaded: wait
        if (b.IsAir()) continue;
        if (StopsDrill(b)) {
            drill.state = Power::DrillState::Blocked;
            drill.depth = depth;
            return false;
        }
        world->SetBlock(p.x, p.y, p.z, Block());
        ChestItems& chest = ChestAt(*chestAt);
        chest[static_cast<size_t>(b.ResourceType())] += static_cast<uint32_t>(b.ResourceUnits());
        SendChestToViewers(*chestAt);
        drill.depth = depth;
        drill.state = Power::DrillState::Working;
        return true;
    }
    drill.state = Power::DrillState::Done;
    return false;
}

void Server::SendMachines() {
    constexpr float kSendRange = 96.0f;
    for (auto& [peer, client] : clients) {
        if (!client.welcomed) continue;
        net::Machines m;
        for (const auto& pos : machineList) {
            if (glm::distance(glm::vec3(pos) + 0.5f, client.feet) > kSendRange) continue;
            net::MachineInfo info;
            info.pos = pos;
            auto n = networkOf.find(pos);
            info.pressure = n != networkOf.end() ? ToByte(networks[n->second].Pressure()) : 0;
            if (auto w = wheels.find(pos); w != wheels.end()) {
                info.type = static_cast<uint8_t>(BlockType::WATER_WHEEL);
                info.speed = ToByte(w->second.speed);
            }
            else if (auto d = drills.find(pos); d != drills.end()) {
                info.type = static_cast<uint8_t>(BlockType::DRILL);
                info.state = static_cast<uint8_t>(d->second.state);
                info.depth = static_cast<uint8_t>(std::clamp(d->second.depth, 0, 255));
            }
            else continue;
            m.machines.push_back(info);
        }
        if (m.machines.empty() && !client.sentMachines) continue;
        client.sentMachines = !m.machines.empty();
        net.Send(peer, net::kChannelUnreliable, net::Encode(m));
    }
}

void Server::HandleInspect(ClientState& client, const glm::ivec3& pos) {
    Block b;
    if (!InReach(client, pos, 8.0f) || !world->GetBlock(pos.x, pos.y, pos.z, b) || !Power::IsPart(b.type)) return;
    char line[256];
    std::string text;
    if (b.type == BlockType::WATER_WHEEL) {
        const Wheel w = wheels.count(pos) ? wheels[pos] : Wheel{};
        if (w.speed > 0.0f) std::snprintf(line, sizeof line, "Water wheel: turning at %d%% (%.0f water a second past it). ", int(std::lround(w.speed * 100)), w.flow);
        else std::snprintf(line, sizeof line, "Water wheel: still (it needs water flowing past it). ");
        text += line;
    }
    else if (b.type == BlockType::DRILL) {
        const Drill d = drills.count(pos) ? drills[pos] : Drill{};
        std::snprintf(line, sizeof line, "Drill: %s%s. ", Power::Describe(d.state),
                      d.depth > 0 ? (", " + std::to_string(d.depth) + " deep").c_str() : "");
        text += line;
    }
    auto n = networkOf.find(pos);
    if (n == networkOf.end()) {
        text += "Not joined to a water wheel or machine.";
    }
    else {
        const AirNetwork& net = networks[n->second];
        std::snprintf(line, sizeof line, "Air pressure %d%% (%d pipes; wheels turning: %d of %d, +%.1f air/s; drills working: %d of %d)",
                      int(std::lround(net.Pressure() * 100)), net.pipes, net.turning, net.wheels, net.supply, net.working, net.drills);
        text += line;
    }
    SendChat(client, net::ChatMessage::Kind::Info, text);
}
