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

bool Server::WheelClear(const glm::ivec3& pos, const Block& wheel) const {
    const glm::ivec3 across = Power::AcrossOf(wheel);
    for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++) {
            if (i == 0 && j == 0) continue;
            const glm::ivec3 p = pos + across * i + glm::ivec3(0, j, 0);
            Block b;
            if (!world->GetBlock(p.x, p.y, p.z, b)) return false;
            if (!b.IsAir() && !b.IsLiquid() && !b.IsPlant()) return false;
        }
    return true;
}

void Server::UpdatePower() {
    const double now = Now();
    if (now < nextPowerUpdate) return;
    nextPowerUpdate = now + Power::kInterval;

    world->TakePower(machineList, wheelFlow);
    // (Held throughout: pumps' allowances live with the water simulation, and this
    // keeps the blocks still while networks are traced.)
    std::lock_guard<std::recursive_mutex> edit(world->GetEditMutex());
    WaterSimulation& water = world->GetWater();
    auto blockAt = [&](const glm::ivec3& p) {
        Block b;
        if (!world->GetBlock(p.x, p.y, p.z, b)) b.type = BlockType::COUNT;
        return b;
    };

    std::unordered_set<glm::ivec3, Coords::IVec3Hash> present(machineList.begin(), machineList.end());
    auto prune = [&](auto& map) {
        for (auto it = map.begin(); it != map.end();) it = present.count(it->first) ? std::next(it) : map.erase(it);
    };
    prune(wheels);
    prune(drills);
    prune(compressors);
    prune(pumpStates);

    // Wheels: the water through one side against the other. Each unit that moves is
    // counted leaving one cell and arriving in another, over a side's three cells:
    // a stream of F units a second down one side reads F.
    for (const auto& [pos, moved] : wheelFlow) {
        Wheel& w = wheels[pos];
        const float reading = std::abs(moved.x - moved.y) * 0.5f / 3.0f / static_cast<float>(Power::kInterval);
        w.flow += (reading - w.flow) * Power::kWheelSmoothing;
        if (w.flow < 0.5f) w.flow = 0.0f;
        w.jammed = !WheelClear(pos, blockAt(pos));
        w.speed = w.jammed ? 0.0f : Power::WheelSpeed(w.flow);
        w.driving = false;
    }

    // Compressors: the wheels on the shaft lines running into them (each wheel turns
    // only one compressor, the first to find it).
    static const glm::ivec3 kLevel[4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const auto& pos : machineList) {
        if (blockAt(pos).type != BlockType::COMPRESSOR) continue;
        Compressor& c = compressors[pos];
        c = Compressor{};
        for (const auto& d : kLevel)
            for (int k = 1; k <= Power::kMaxShaft; k++) {
                const glm::ivec3 p = pos + d * k;
                const Block b = blockAt(p);
                if ((b.type != BlockType::SHAFT && b.type != BlockType::WATER_WHEEL) || Power::AxleOf(b) != glm::abs(d)) break;
                if (b.type != BlockType::WATER_WHEEL) continue;
                Wheel& w = wheels[p];
                if (w.driving) continue;
                w.driving = true;
                c.wheels++;
                if (w.speed > 0.0f) c.turning++;
                c.power += w.speed;
            }
    }

    // Air networks: everything joined to each compressor and machine.
    std::unordered_map<glm::ivec3, float, Coords::IVec3Hash> newAir;
    networks.clear();
    networkOf.clear();
    std::vector<glm::ivec3> members;
    for (const auto& start : machineList) {
        if (networkOf.count(start) || !Power::IsAirPart(blockAt(start).type)) continue;
        const int index = static_cast<int>(networks.size());
        networks.emplace_back();
        AirNetwork& net = networks.back();
        members.assign(1, start);
        networkOf[start] = index;
        for (size_t i = 0; i < members.size() && members.size() < static_cast<size_t>(Power::kMaxNetwork); i++)
            for (const auto& d : kFaces) {
                const glm::ivec3 n = members[i] + d;
                if (networkOf.count(n) || !Power::IsAirPart(blockAt(n).type)) continue;
                networkOf[n] = index;
                members.push_back(n);
            }

        std::vector<std::pair<glm::ivec3, Drill*>> netDrills;
        std::vector<glm::ivec3> netPumps;
        for (const auto& m : members) {
            auto a = air.find(m);
            if (a != air.end()) net.air += a->second;
            switch (blockAt(m).type) {
            case BlockType::AIR_PIPE: net.pipes++; break;
            case BlockType::AIR_TANK: net.tanks++; break;
            case BlockType::COMPRESSOR:
                net.compressors++;
                net.supply += compressors[m].power * Power::kWheelMaxAir;
                break;
            case BlockType::DRILL:
                net.drills++;
                netDrills.emplace_back(m, &drills[m]);
                break;
            case BlockType::PUMP:
                net.pumps++;
                netPumps.push_back(m);
                break;
            default: break;
            }
        }
        for (const auto& m : members) net.capacity += Power::CapacityOf(blockAt(m).type);
        net.air = std::min(net.capacity, net.air + net.supply * static_cast<float>(Power::kInterval));

        // Pumps: the air for what they lifted since the last update.
        for (const auto& p : netPumps) {
            const WaterSimulation::PumpUse use = water.TakePumpUse(p);
            net.air = std::max(0.0f, net.air - use.lifted * Power::kPumpAir);
            Power::DrillState& state = pumpStates[p];
            state = !use.wanted ? Power::DrillState::Idle : use.lifted > 0.0f ? Power::DrillState::Working : Power::DrillState::NoPressure;
            if (state == Power::DrillState::Working) net.pumping++;
        }

        // Drills take their turn: whichever has waited longest goes first.
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

        // What the pumps may lift until the next update: the air above the running
        // pressure, less what the drills need for their next blocks (they come first),
        // shared between them (none below it).
        const float reserve = Power::kMinPressure * net.capacity + Power::kDrillAir * static_cast<float>(netDrills.size());
        const float spare = std::max(0.0f, net.air - reserve);
        for (const auto& p : netPumps)
            water.SetPumpBudget(p, net.Pressure() >= Power::kMinPressure ? spare / static_cast<float>(netPumps.size()) / Power::kPumpAir : 0.0f);

        // Everything at the same pressure: each part holds its share by size.
        const float fill = net.capacity > 0.0f ? net.air / net.capacity : 0.0f;
        for (const auto& m : members) newAir[m] = fill * Power::CapacityOf(blockAt(m).type);
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
                info.state = static_cast<uint8_t>(w->second.jammed ? Power::DrillState::Blocked
                                                  : w->second.speed > 0.0f ? Power::DrillState::Working : Power::DrillState::Idle);
            }
            else if (auto d = drills.find(pos); d != drills.end()) {
                info.type = static_cast<uint8_t>(BlockType::DRILL);
                info.state = static_cast<uint8_t>(d->second.state);
                info.depth = static_cast<uint8_t>(std::clamp(d->second.depth, 0, 255));
            }
            else if (auto c = compressors.find(pos); c != compressors.end()) {
                // speed: air a second, in halves; depth: wheels on its shafts.
                info.type = static_cast<uint8_t>(BlockType::COMPRESSOR);
                info.speed = static_cast<uint8_t>(std::clamp(std::lround(c->second.power * Power::kWheelMaxAir * 2.0f), 0L, 255L));
                info.depth = static_cast<uint8_t>(std::clamp(c->second.wheels, 0, 255));
                info.state = static_cast<uint8_t>(c->second.turning ? Power::DrillState::Working : Power::DrillState::Idle);
            }
            else if (Block t; world->GetBlock(pos.x, pos.y, pos.z, t) && t.type == BlockType::AIR_TANK) {
                info.type = static_cast<uint8_t>(BlockType::AIR_TANK);
            }
            else if (auto p = pumpStates.find(pos); p != pumpStates.end()) {
                info.type = static_cast<uint8_t>(BlockType::PUMP);
                info.state = static_cast<uint8_t>(p->second);
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
        if (w.jammed) std::snprintf(line, sizeof line, "Water wheel: jammed (its paddles need the 8 cells around its hub clear). ");
        else if (w.speed > 0.0f) std::snprintf(line, sizeof line, "Water wheel: turning at %d%% (%.0f water a second down one side). ", int(std::lround(w.speed * 100)), w.flow);
        else std::snprintf(line, sizeof line, "Water wheel: still (it needs water flowing down one side). ");
        text += line;
        text += w.driving ? "It turns a compressor." : "Its shaft doesn't reach a compressor.";
    }
    else if (b.type == BlockType::SHAFT) {
        text += "Shaft: carries wheels' turning along its axle to a compressor at the end of the line.";
    }
    else {
        if (b.type == BlockType::DRILL) {
            const Drill d = drills.count(pos) ? drills[pos] : Drill{};
            std::snprintf(line, sizeof line, "Drill: %s%s. ", Power::Describe(d.state), d.depth > 0 ? (", " + std::to_string(d.depth) + " deep").c_str() : "");
            text += line;
        }
        else if (b.type == BlockType::COMPRESSOR) {
            const Compressor c = compressors.count(pos) ? compressors[pos] : Compressor{};
            std::snprintf(line, sizeof line, "Compressor: wheels turning %d of %d on its shafts, +%.1f air/s. ", c.turning, c.wheels, c.power * Power::kWheelMaxAir);
            text += line;
        }
        else if (b.type == BlockType::AIR_TANK) {
            const float held = air.count(pos) ? air[pos] : 0.0f;
            std::snprintf(line, sizeof line, "Air tank: holding %.0f of %.0f. ", held, Power::kAirPerTank);
            text += line;
        }
        else if (b.type == BlockType::VALVE || b.type == BlockType::VALVE_OPEN) {
            text += b.type == BlockType::VALVE_OPEN ? "Valve: open. " : "Valve: shut (the pipes either side are kept apart). ";
        }
        else if (b.type == BlockType::PUMP) {
            const Power::DrillState st = pumpStates.count(pos) ? pumpStates[pos] : Power::DrillState::Idle;
            text += st == Power::DrillState::Working ? "Pump: pumping. " : st == Power::DrillState::NoPressure ? "Pump: waiting for air pressure. " : "Pump: nothing to pump. ";
        }
        auto n = networkOf.find(pos);
        if (n == networkOf.end()) text += "Not joined to a compressor or machine.";
        else {
            const AirNetwork& net = networks[n->second];
            std::snprintf(line, sizeof line, "Air pressure %d%% (%.0f of %.0f air; %d pipes, %d tanks; compressors: %d, +%.1f air/s; drills working: %d of %d; pumps working: %d of %d)",
                          int(std::lround(net.Pressure() * 100)), net.air, net.capacity, net.pipes, net.tanks, net.compressors, net.supply, net.working, net.drills, net.pumping, net.pumps);
            text += line;
        }
    }
    SendChat(client, net::ChatMessage::Kind::Info, text);
}
