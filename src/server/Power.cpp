// Air power: water wheels, air pipes and machines (see world/Power.h).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include "server/Server.h"
#include "world/Breathing.h"
#include "server/ServerWorld.h"
#include "world/Crafting.h"
#include "world/Lava.h"
#include "world/Multiblocks.h"
#include "world/Smelting.h"

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
    case BlockType::MINING_POST:
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
    prune(pistonStates);
    prune(smelters);
    prune(workers);
    prune(engines);
    // Formed structures no longer whole come apart; the rest are found by their air.
    std::unordered_map<glm::ivec3, glm::ivec3, Coords::IVec3Hash> structureAtAir;
    for (const auto& pos : machineList) {
        const Block b = blockAt(pos);
        if (b.type != BlockType::MULTIBLOCK) continue;
        if (!StructureWhole(pos, b)) {
            TakeApart(pos);
            continue;
        }
        glm::ivec3 port;
        if (Multiblocks::PortOf(pos, b, Multiblocks::Role::AirPort, port)) structureAtAir[port] = pos;
    }
    BuildFluidNets();

    // A piston's head with no piston behind it (broken, or turned) goes too.
    for (const auto& pos : machineList) {
        const Block head = blockAt(pos);
        if (head.type != BlockType::PISTON_HEAD) continue;
        const Block body = blockAt(pos - head.Facing());
        if (body.type == BlockType::COUNT) continue; // (not loaded: leave it)
        if (body.type != BlockType::PISTON || body.axis != head.axis) world->SetBlock(pos.x, pos.y, pos.z, Block());
    }

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
    // (Traced from every machine on a network, and from structures' air ports: a
    // structure isn't part of the network itself.)
    std::vector<glm::ivec3> starts = machineList;
    for (const auto& [port, core] : structureAtAir) starts.push_back(port);
    for (const auto& start : starts) {
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
        std::vector<glm::ivec3> netPumps, netPistons, netSmelters, netWorkers, netStructures, netDiffusers, netLaunchers;
        for (const auto& m : members) {
            auto a = air.find(m);
            if (a != air.end()) net.air += a->second;
            if (auto st = structureAtAir.find(m); st != structureAtAir.end()) {
                Block core;
                world->GetBlock(st->second.x, st->second.y, st->second.z, core);
                if (Multiblocks::KindOf(core) == Multiblocks::Kind::DieselCompressor) {
                    // A diesel compressor makes air (while it has fuel), like an engine.
                    net.compressors++;
                    Worker& w = workers[st->second];
                    glm::ivec3 port;
                    FluidNet* fuel = Multiblocks::PortOf(st->second, core, Multiblocks::Role::FluidIn, port) ? FluidNetAt(port) : nullptr;
                    const int burn = static_cast<int>(Multiblocks::kDieselFuel * Power::kInterval);
                    if (fuel && fuel->kind == Fluids::Kind::Fuel && fuel->amount >= burn) {
                        fuel->amount -= burn;
                        net.supply += Multiblocks::kDieselAir;
                        w.state = Power::DrillState::Working;
                        w.last = now;
                    }
                    else w.state = Power::DrillState::Idle;
                }
                else netStructures.push_back(st->second);
            }
            switch (blockAt(m).type) {
            case BlockType::AIR_PIPE: net.pipes++; break;
            case BlockType::AIR_TANK: net.tanks++; break;
            case BlockType::CREATIVE_AIR_TANK: net.tanks++, net.endless = true; break;
            case BlockType::COMPRESSOR:
                net.compressors++;
                net.supply += compressors[m].power * Power::kWheelMaxAir;
                break;
            case BlockType::AIR_DIFFUSER: netDiffusers.push_back(m); break;
            case BlockType::TURBINE: {
                // A steam turbine runs while it has steam, making air like a compressor.
                net.compressors++;
                const int use = static_cast<int>(Fluids::kTurbineSteam * Power::kInterval);
                FluidNet* steam = FluidNetBeside(m, Fluids::Kind::Steam, true);
                const bool running = steam && steam->amount >= use;
                if (running) {
                    steam->amount -= use;
                    net.supply += Fluids::kTurbineAir;
                }
                engines[m] = running;
                break;
            }
            case BlockType::ENGINE: {
                // An engine runs while it has fuel to burn, making air like a compressor.
                net.compressors++;
                const int burn = static_cast<int>(Fluids::kEngineFuel * Power::kInterval);
                FluidNet* fuel = FluidNetBeside(m, Fluids::Kind::Fuel, true);
                const bool running = fuel && fuel->amount >= burn;
                if (running) {
                    fuel->amount -= burn;
                    net.supply += Fluids::kEngineAir;
                }
                engines[m] = running;
                break;
            }
            case BlockType::DRILL:
                net.drills++;
                netDrills.emplace_back(m, &drills[m]);
                break;
            case BlockType::PUMP:
                net.pumps++;
                netPumps.push_back(m);
                break;
            case BlockType::PISTON: netPistons.push_back(m); break;
            case BlockType::LAUNCHER: netLaunchers.push_back(m); break;
            case BlockType::SMELTER:
            case BlockType::SMELTER_LIT: netSmelters.push_back(m); break;
            case BlockType::EXTRACTOR:
            case BlockType::CRAFTER:
            case BlockType::HARVESTER:
            case BlockType::OIL_DERRICK:
            case BlockType::CRUSHER:
            case BlockType::REFINERY: netWorkers.push_back(m); break;
            default: break;
            }
        }
        for (const auto& m : members) net.capacity += Power::CapacityOf(blockAt(m).type);
        net.air = std::min(net.capacity, net.air + net.supply * static_cast<float>(Power::kInterval));
        if (net.endless) net.air = net.capacity;

        // Air diffusers breathe out fresh air while the network has the pressure for it.
        for (const auto& d : netDiffusers) {
            const float use = Breathing::kDiffuserAir * static_cast<float>(Power::kInterval);
            const bool running = net.Pressure() >= Power::kMinPressure && net.air >= use;
            if (running) net.air -= use;
            diffusers[d] = running;
        }

        // Pumps: the air for what they lifted since the last update.
        for (const auto& p : netPumps) {
            const WaterSimulation::PumpUse use = water.TakePumpUse(p);
            net.air = std::max(0.0f, net.air - use.lifted * Power::kPumpAir);
            Power::DrillState& state = pumpStates[p];
            state = !use.wanted ? Power::DrillState::Idle : use.lifted > 0.0f ? Power::DrillState::Working : Power::DrillState::NoPressure;
            if (state == Power::DrillState::Working) net.pumping++;
        }

        // Pistons go out while their channel is on (if there's the air), and back in
        // when it's off.
        for (const auto& p : netPistons) {
            const Block piston = blockAt(p);
            auto sig = signalBlocks.find(p);
            const bool want = sig != signalBlocks.end() && sig->second.on;
            const Block front = blockAt(p + piston.Facing());
            const bool out = front.type == BlockType::PISTON_HEAD && front.axis == piston.axis;
            Power::DrillState& state = pistonStates[p];
            if (want && !out) {
                if (net.Pressure() < Power::kMinPressure || net.air < Power::kPistonAir) state = Power::DrillState::NoPressure;
                else if (PistonPush(p, piston)) {
                    net.air -= Power::kPistonAir;
                    state = Power::DrillState::Working;
                }
                else state = Power::DrillState::Blocked;
            }
            else if (!want && out) {
                PistonPull(p, piston);
                state = Power::DrillState::Idle;
            }
            else state = out ? Power::DrillState::Working : Power::DrillState::Idle;
        }

        // Air launchers fire as their channel comes on (if there's the air).
        for (const auto& p : netLaunchers) {
            auto sig = signalBlocks.find(p);
            const bool want = sig != signalBlocks.end() && sig->second.on;
            bool& armed = launcherArmed[p];
            const float speed = Power::LaunchSpeed(sig != signalBlocks.end() ? sig->second.config.b : 0);
            if (want && !armed && net.Pressure() >= Power::kMinPressure && net.air >= Power::LaunchAir(speed)) {
                net.air -= Power::LaunchAir(speed);
                Launch(p, blockAt(p), speed);
            }
            armed = want;
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

        // Smelters too: a block each in turn, lit while they work.
        for (const auto& p : netSmelters) {
            Smelter& sm = smelters[p];
            if (now >= sm.nextSmelt) {
                if (net.Pressure() < Power::kMinPressure || net.air < Power::kSmeltAir) sm.state = Power::DrillState::NoPressure;
                else if (Smelt(p, sm)) {
                    net.air -= Power::kSmeltAir;
                    sm.nextSmelt = now + Power::kSmeltInterval;
                    sm.lastSmelt = now;
                }
                else sm.nextSmelt = now + 0.5; // (look again soon)
            }
            if (now - sm.lastSmelt < Power::kSmeltInterval * 1.5) sm.state = Power::DrillState::Working;
            if (sm.state == Power::DrillState::Working) net.working++;
            const BlockType shown = sm.state == Power::DrillState::Working ? BlockType::SMELTER_LIT : BlockType::SMELTER;
            if (blockAt(p).type != shown) world->SetBlock(p.x, p.y, p.z, Block(shown));
        }

        // Extractors, crafters and harvesters, each at its own pace.
        for (const auto& p : netWorkers) {
            Worker& w = workers[p];
            const Block b = blockAt(p);
            double interval = Power::kHarvestInterval;
            float cost = Power::kHarvestAir;
            switch (b.type) {
            case BlockType::EXTRACTOR: interval = Power::kExtractInterval, cost = Power::kExtractAir; break;
            case BlockType::CRAFTER: interval = Power::kCraftInterval, cost = Power::kCraftAir; break;
            case BlockType::OIL_DERRICK: interval = Fluids::kDerrickInterval, cost = Fluids::kDerrickAir; break;
            case BlockType::REFINERY: interval = Power::kInterval * 0.9, cost = Fluids::kRefineAir; break;
            case BlockType::CRUSHER: interval = Power::kCrushInterval, cost = Power::kCrushAir; break;
            default: break;
            }
            auto work = [&] {
                switch (b.type) {
                case BlockType::EXTRACTOR: return Extract(p, b, w);
                case BlockType::CRAFTER: return CraftAt(p, b, w);
                case BlockType::OIL_DERRICK: return PumpOil(p, w);
                case BlockType::REFINERY: return Refine(p, b, w);
                case BlockType::CRUSHER: return Crush(p, w);
                default: return Harvest(p, w);
                }
            };
            if (now >= w.next) {
                if (net.Pressure() < Power::kMinPressure || net.air < cost) w.state = Power::DrillState::NoPressure;
                else if (work()) {
                    net.air -= cost;
                    w.next = now + interval;
                    w.last = now;
                }
                else w.next = now + 0.5;
            }
            if (now - w.last < interval * 1.5) w.state = Power::DrillState::Working;
            if (w.state == Power::DrillState::Working) net.working++;
        }

        // Formed structures, each at its own pace (the air comes in at their air port).
        for (const auto& core : netStructures) {
            Worker& w = workers[core];
            const Block b = blockAt(core);
            const Multiblocks::Kind kind = Multiblocks::KindOf(b);
            const double interval = kind == Multiblocks::Kind::Refinery     ? Power::kInterval * 0.9
                                    : kind == Multiblocks::Kind::OilDerrick ? Fluids::kDerrickInterval
                                    : kind == Multiblocks::Kind::CokeOven   ? Multiblocks::kCokeInterval
                                                                            : kFurnaceInterval;
            const float cost = kind == Multiblocks::Kind::Refinery ? 4.0f : 3.0f;
            if (now >= w.next) {
                if (net.Pressure() < Power::kMinPressure || net.air < cost) w.state = Power::DrillState::NoPressure;
                else if (RunStructure(core, b, w)) {
                    net.air -= cost;
                    w.next = now + interval;
                    w.last = now;
                }
                else w.next = now + 0.5;
            }
            if (now - w.last < interval * 1.5) w.state = Power::DrillState::Working;
            if (w.state == Power::DrillState::Working) net.working++;
        }

        // What the pumps may lift until the next update: the air above the running
        // pressure, less what the drills and smelters need for their next blocks (they
        // come first), shared between them (none below it).
        const float reserve = Power::kMinPressure * net.capacity + Power::kDrillAir * static_cast<float>(netDrills.size()) +
                              Power::kSmeltAir * static_cast<float>(netSmelters.size());
        const float spare = net.endless ? 1e6f : std::max(0.0f, net.air - reserve);
        for (const auto& p : netPumps)
            water.SetPumpBudget(p, net.Pressure() >= Power::kMinPressure ? spare / static_cast<float>(netPumps.size()) / Power::kPumpAir : 0.0f);

        // Everything at the same pressure: each part holds its share by size (and a
        // creative tank fills it all again).
        if (net.endless) net.air = net.capacity;
        const float fill = net.capacity > 0.0f ? net.air / net.capacity : 0.0f;
        for (const auto& m : members) newAir[m] = fill * Power::CapacityOf(blockAt(m).type);
    }
    // Parts in chunks that aren't loaded keep their air until they're back, when their
    // network takes it in again. (Parts gone, or loaded but in no network, lose theirs.)
    for (const auto& [pos, held] : air) {
        Block b;
        if (!networkOf.count(pos) && !world->GetBlock(pos.x, pos.y, pos.z, b)) newAir[pos] = held;
    }
    air.swap(newAir);

    // Drains and fluid outlets: the water they traded since the last update into or out
    // of their pipes, and what they may trade until the next.
    for (const auto& pos : machineList) {
        const BlockType t = blockAt(pos).type;
        if (t == BlockType::PUMP) {
            // Pipes holding another fluid (oil, fuel, steam) aren't for its water: it stands.
            bool otherFluid = false;
            for (const auto& d : kFaces)
                if (const FluidNet* n = FluidNetAt(pos + d); n && n->amount > 0 && n->kind != Fluids::Kind::Water) otherFluid = true;
            if (otherFluid) {
                water.TakeDrainUse(pos);
                water.SetDrainBudget(pos, -1); // (server/Water.h: a pump told -1 stands)
                continue;
            }
            // A pump whose pipes reach a tank or machine fills them (server/Water.h); one
            // whose pipes just end lets its water out there, and puts none in.
            if (FluidNet* in = FluidNetBeside(pos, Fluids::Kind::Water, false)) {
                in->amount = std::min(in->capacity, in->amount + water.TakeDrainUse(pos));
                if (in->amount > 0) in->kind = Fluids::Kind::Water;
                water.SetDrainBudget(pos, in->Room());
            }
            else {
                water.TakeDrainUse(pos);
                water.SetDrainBudget(pos, 0);
            }
        }
        else if (t == BlockType::DRAIN) {
            if (FluidNet* in = FluidNetBeside(pos, Fluids::Kind::Water, false)) {
                in->amount = std::min(in->capacity, in->amount + water.TakeDrainUse(pos));
                if (in->amount > 0) in->kind = Fluids::Kind::Water;
                water.SetDrainBudget(pos, in->Room());
            }
            else {
                water.TakeDrainUse(pos);
                water.SetDrainBudget(pos, 0);
            }
        }
        else if (t == BlockType::BOILER) {
            // Water under it, steam over it, while there's fire.
            Worker& w = workers[pos];
            FluidNet* in = FluidNetAt(pos - glm::ivec3(0, 1, 0));
            FluidNet* out = FluidNetAt(pos + glm::ivec3(0, 1, 0));
            // (Lava in obsidian pipes beside it fires it instead of coal.)
            FluidNet* lava = nullptr;
            for (const auto& d : kFaces)
                if (FluidNet* n = FluidNetAt(pos + d); n && n->kind == Fluids::Kind::Lava && n->amount > 0) lava = n;
            if (!in || !out || in == out) w.state = Power::DrillState::NoChest;
            else if (in->kind != Fluids::Kind::Water || in->amount <= 0 || !out->Takes(Fluids::Kind::Steam) || out->Room() <= 0) w.state = Power::DrillState::Idle;
            else {
                bool fire = now < w.next;
                if (!fire && lava) {
                    const int burn = std::max(1, static_cast<int>(Fluids::kLavaBurn * Power::kInterval));
                    if (lava->amount >= burn) lava->amount -= burn, fire = true;
                }
                if (!fire)
                    // (Coke first: it lasts longer.)
                    for (const BlockType fuel : { BlockType::COKE, BlockType::COAL }) {
                        for (const auto& c : ChestsJoined(pos)) {
                            if (!ChestAt(c).TakeUnits(fuel, Block::kUnitsPerBlock)) continue;
                            SendChestToViewers(c);
                            w.next = now + (fuel == BlockType::COKE ? Fluids::kCokeBurn : Fluids::kCoalBurn);
                            fire = true;
                            break;
                        }
                        if (fire) break;
                    }
                if (!fire) w.state = Power::DrillState::NoPressure;
                else {
                    const int units = std::min({ static_cast<int>(Fluids::kBoilRate * Power::kInterval), in->amount, out->Room() });
                    in->amount -= units;
                    out->amount += units;
                    out->kind = Fluids::Kind::Steam;
                    w.state = Power::DrillState::Working;
                    w.last = now;
                }
            }
        }
        else if (t == BlockType::FLUID_OUTLET) {
            if (FluidNet* from = FluidNetBeside(pos, Fluids::Kind::Water, true)) {
                from->amount = std::max(0, from->amount - water.TakeDrainUse(pos));
                water.SetDrainBudget(pos, from->amount);
            }
            else {
                water.TakeDrainUse(pos);
                water.SetDrainBudget(pos, 0);
            }
        }
    }
    // Obsidian pipes touching lava draw it in, as far as their network has room.
    {
        const int perPipe = static_cast<int>(Fluids::kLavaIntake * Power::kInterval);
        for (FluidNet& fn : fluidNets) {
            if (!fn.lavaProof || !fn.Takes(Fluids::Kind::Lava)) continue;
            for (const auto& m : fn.members)
                for (const auto& d : kFaces) {
                    if (fn.Room() <= 0) break;
                    const glm::ivec3 c = m + d;
                    const Block lavaCell = blockAt(c);
                    if (!lavaCell.IsLava()) continue;
                    const int take = std::min({ perPipe, Lava::Volume(lavaCell), fn.Room() });
                    world->SetBlock(c.x, c.y, c.z, Lava::WithVolume(Lava::Volume(lavaCell) - take));
                    fn.amount += take;
                    fn.kind = Fluids::Kind::Lava;
                }
        }
    }
    // Steam lets itself out into the air: out of the end face of every open end of its
    // pipes, and out of the fluid outlets on its network (world/Fluids.h).
    {
        const int perEnd = static_cast<int>(Fluids::kVentRate * Power::kInterval);
        const int perOutlet = static_cast<int>(Fluids::kOutletVentRate * Power::kInterval);
        auto vent = [&](FluidNet& fn, const glm::ivec3& out, int most) {
            if (fn.amount <= 0 || !SteamCloud::Open(*world, out)) return;
            const int take = std::min(most, fn.amount);
            fn.amount -= take;
            steam.Add(out, take * Fluids::kSteamGas);
        };
        for (FluidNet& fn : fluidNets) {
            if (fn.kind != Fluids::Kind::Steam || fn.amount <= 0) continue;
            for (const auto& m : fn.members) {
                if (blockAt(m).type != BlockType::FLUID_PIPE && blockAt(m).type != BlockType::FLUID_VALVE_OPEN) continue;
                int joins = 0;
                glm::ivec3 joinedAt(0);
                for (const auto& d : kFaces)
                    if (Power::PipeJoins(BlockType::FLUID_PIPE, blockAt(m + d).type)) joins++, joinedAt = d;
                if (joins == 1) vent(fn, m - joinedAt, perEnd);
            }
        }
        for (const auto& pos : machineList) {
            const Block outlet = blockAt(pos);
            if (outlet.type != BlockType::FLUID_OUTLET) continue;
            if (FluidNet* from = FluidNetBeside(pos, Fluids::Kind::Steam, true)) vent(*from, pos + outlet.Facing(), perOutlet);
        }
    }
    StoreFluids();
    SendMachines();
}

bool Server::FreshAirAt(const glm::vec3& eye) const {
    for (const auto& [pos, running] : diffusers)
        if (running && glm::distance(glm::vec3(pos) + 0.5f, eye) <= Breathing::kDiffuserRange) {
            Block b;
            if (world->GetBlock(pos.x, pos.y, pos.z, b) && b.type == BlockType::AIR_DIFFUSER) return true;
        }
    return false;
}

bool Server::Crush(const glm::ivec3& pos, Worker& w) {
    const auto joined = ChestsJoined(pos);
    if (joined.empty()) {
        w.state = Power::DrillState::NoChest;
        return false;
    }
    // Iron ore first (into twice as much crushed iron), then stone (into sand).
    const std::pair<BlockType, std::pair<BlockType, uint32_t>> grinds[] = {
        { BlockType::IRON_ORE, { BlockType::CRUSHED_IRON, 2 * Block::kUnitsPerBlock } },
        { BlockType::STONE, { BlockType::SAND, Block::kUnitsPerBlock } },
    };
    bool full = false;
    for (const auto& [from, to] : grinds)
        for (const auto& c : joined) {
            Inventory& chest = ChestAt(c);
            if (chest.Units(from) < Block::kUnitsPerBlock) continue;
            // (What it grinds is gone first, so it may make the room.)
            Inventory after = chest;
            after.TakeUnits(from, Block::kUnitsPerBlock);
            if (after.AddUnits(to.first, to.second) != 0) {
                full = true;
                continue;
            }
            chest = std::move(after);
            SendChestToViewers(c);
            w.state = Power::DrillState::Working;
            return true;
        }
    w.state = full ? Power::DrillState::Full : Power::DrillState::Idle;
    return false;
}

std::vector<glm::ivec3> Server::ChestsJoined(const glm::ivec3& pos, BlockType type, const glm::ivec3* face) const {
    std::vector<glm::ivec3> chestsFound, pipes{ pos };
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> seen{ pos };
    // A machine with an inventory of its own: that first.
    if (Block self; !face && world->GetBlock(pos.x, pos.y, pos.z, self) && IsMachineInventory(self.type)) chestsFound.push_back(pos);
    for (size_t i = 0; i < pipes.size() && pipes.size() < static_cast<size_t>(Power::kMaxItemNetwork); i++)
        for (const auto& d : kFaces) {
            if (i == 0 && face && d != *face) continue;
            const glm::ivec3 n = pipes[i] + d;
            Block b;
            if (seen.count(n) || !world->GetBlock(n.x, n.y, n.z, b)) continue;
            seen.insert(n);
            if (IsStorageType(b.type)) chestsFound.push_back(n);
            else if (b.type == BlockType::ITEM_PIPE) pipes.push_back(n);
            else if (b.type == BlockType::FILTER_PIPE && (b.FilterType() == BlockType::AIR || (type != BlockType::COUNT && b.FilterType() == type))) pipes.push_back(n);
        }
    return chestsFound;
}

bool Server::Extract(const glm::ivec3& pos, const Block& extractor, Worker& w) {
    const glm::ivec3 from = pos + extractor.Facing(), back = -extractor.Facing();
    Block source;
    if (!world->GetBlock(from.x, from.y, from.z, source) || !IsStorageType(source.type)) {
        w.state = Power::DrillState::NoChest;
        return false;
    }
    // The kinds of things in the chest, in order (whole items: offcuts stay).
    std::vector<ItemId> kinds;
    for (const Stack& st : ChestAt(from).slots)
        if (!st.Empty()) kinds.push_back(st.item);
    std::sort(kinds.begin(), kinds.end());
    kinds.erase(std::unique(kinds.begin(), kinds.end()), kinds.end());
    // Through them in turn (after the last one it moved), the first thing somewhere will take.
    const size_t start = static_cast<size_t>(std::upper_bound(kinds.begin(), kinds.end(), w.cursor) - kinds.begin());
    for (size_t k = 0; k < kinds.size(); k++) {
        const ItemId item = kinds[(start + k) % kinds.size()];
        const BlockType type = Items::IsBlock(item) ? Items::BlockOf(item) : BlockType::COUNT; // (filters pass blocks of their kind)
        std::vector<glm::ivec3> to = ChestsJoined(pos, type, &back);
        to.erase(std::remove(to.begin(), to.end(), from), to.end());
        to.erase(std::remove_if(to.begin(), to.end(), [&](const glm::ivec3& c) { return ChestAt(c).Room(item) == 0; }), to.end());
        if (to.empty()) continue;
        // Where it goes: a chest behind a filter set to it (one already holding some
        // first), else one already holding some, else the nearest.
        const std::vector<glm::ivec3> open = ChestsJoined(pos, BlockType::COUNT, &back); // (through unset filters only)
        auto filtered = [&](const glm::ivec3& c) { return std::find(open.begin(), open.end(), c) == open.end(); };
        auto pick = [&](bool needFilter, bool needSome) -> std::optional<glm::ivec3> {
            for (const auto& c : to)
                if ((!needFilter || filtered(c)) && (!needSome || ChestAt(c).Count(item) > 0)) return c;
            return std::nullopt;
        };
        glm::ivec3 dest = to.front();
        if (auto c = pick(true, true)) dest = *c;
        else if (auto c2 = pick(true, false)) dest = *c2;
        else if (auto c3 = pick(false, true)) dest = *c3;
        ChestAt(from).Take(item, 1);
        ChestAt(dest).Add(item, 1);
        SendChestToViewers(from);
        SendChestToViewers(dest);
        w.cursor = item;
        w.state = Power::DrillState::Working;
        return true;
    }
    w.state = Power::DrillState::Idle;
    return false;
}

bool Server::CraftAt(const glm::ivec3& pos, const Block& crafter, Worker& w) {
    const auto& recipes = Crafting::Recipes();
    const int index = static_cast<int>(crafter.axis) - 1;
    if (index < 0 || index >= static_cast<int>(recipes.size()) || recipes[index].MakesItem()) {
        w.state = Power::DrillState::Blocked; // (no recipe)
        return false;
    }
    const Recipe& recipe = recipes[index];
    const auto joined = ChestsJoined(pos);
    if (joined.empty()) {
        w.state = Power::DrillState::NoChest;
        return false;
    }
    bool full = false;
    for (const auto& c : joined) {
        Inventory& chest = ChestAt(c);
        if (Crafting::TimesAffordable(recipe, chest) < 1) continue;
        Inventory after = chest;
        for (const auto& in : recipe.inputs) after.TakeUnits(in.type, in.units);
        if (after.AddUnits(recipe.output, recipe.outputUnits) != 0) {
            full = true;
            continue;
        }
        chest = std::move(after);
        SendChestToViewers(c);
        w.state = Power::DrillState::Working;
        return true;
    }
    w.state = full ? Power::DrillState::Full : Power::DrillState::Idle;
    return false;
}

bool Server::Harvest(const glm::ivec3& pos, Worker& w) {
    // (A chest joined to it first; its own inventory if there's none.)
    auto joined = ChestsJoined(pos, BlockType::WHEAT);
    if (joined.size() > 1 && joined.front() == pos) std::rotate(joined.begin(), joined.begin() + 1, joined.end());
    if (joined.empty()) {
        w.state = Power::DrillState::NoChest;
        return false;
    }
    // (Somewhere with room for the wheat.)
    joined.erase(std::remove_if(joined.begin(), joined.end(), [&](const glm::ivec3& c) { return !ChestAt(c).FitsUnits(BlockType::WHEAT, Block::kUnitsPerBlock); }), joined.end());
    if (joined.empty()) {
        w.state = Power::DrillState::Full;
        return false;
    }
    const int r = Power::kHarvestReach;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -r; dx <= r; dx++)
            for (int dz = -r; dz <= r; dz++) {
                const glm::ivec3 p = pos + glm::ivec3(dx, dy, dz);
                Block b;
                if (!world->GetBlock(p.x, p.y, p.z, b) || b.type != BlockType::RIPE_WHEAT) continue;
                // Reaped and replanted from its own seed: a young crop again.
                world->SetBlock(p.x, p.y, p.z, Block::Plant(BlockType::CROP, b.edgeData.edges[0]));
                crops.insert(p);
                ChestAt(joined.front()).AddUnits(BlockType::WHEAT, Block::kUnitsPerBlock);
                SendChestToViewers(joined.front());
                w.state = Power::DrillState::Working;
                return true;
            }
    w.state = Power::DrillState::Idle;
    return false;
}

bool Server::Smelt(const glm::ivec3& pos, Smelter& smelter) {
    const auto joined = ChestsJoined(pos);
    if (joined.empty()) {
        smelter.state = Power::DrillState::NoChest;
        return false;
    }
    bool full = false;
    for (const auto& c : joined) {
        Inventory& chest = ChestAt(c);
        const SmeltRecipe* r = Smelting::FirstAffordable(chest);
        if (!r) continue;
        if (!Smelting::Apply(*r, chest)) {
            full = true;
            continue;
        }
        SendChestToViewers(c);
        smelter.made = r->output;
        smelter.state = Power::DrillState::Working;
        return true;
    }
    smelter.state = full ? Power::DrillState::Full : Power::DrillState::Idle;
    return false;
}

bool Server::DrillDig(const glm::ivec3& pos, Drill& drill) {
    // Somewhere to put what it digs: a chest touching it or joined to it by item pipes
    // (not under it, where it digs).
    std::optional<glm::ivec3> chestAt;
    {
        // (What it would dig next decides which filters it may go through.)
        BlockType next = BlockType::COUNT;
        for (int depth = 1; depth <= Power::kDrillDepth && next == BlockType::COUNT; depth++) {
            Block b;
            if (!world->GetBlock(pos.x, pos.y - depth, pos.z, b)) break;
            if (!b.IsAir()) next = b.ResourceType();
        }
        // (A chest joined to it first; its own inventory if there's none.)
        auto found = ChestsJoined(pos, next);
        if (found.size() > 1 && found.front() == pos) std::rotate(found.begin(), found.begin() + 1, found.end());
        bool any = false;
        for (const auto& c : found)
            if (c != pos - glm::ivec3(0, 1, 0)) {
                any = true;
                // (Room for a whole block of it: it may be cut down to less.)
                if (next != BlockType::COUNT && !ChestAt(c).FitsUnits(next, Block::kUnitsPerBlock)) continue;
                chestAt = c;
                break;
            }
        if (any && !chestAt) {
            drill.state = Power::DrillState::Full;
            return false;
        }
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
        ChestAt(*chestAt).AddUnits(b.ResourceType(), static_cast<uint32_t>(b.ResourceUnits()));
        SendChestToViewers(*chestAt);
        drill.depth = depth;
        drill.state = Power::DrillState::Working;
        return true;
    }
    drill.state = Power::DrillState::Done;
    return false;
}

void Server::Launch(const glm::ivec3& pos, const Block& launcher, float speed) {
    const double now = Now();
    launcherFired[pos] = now;
    const glm::vec3 dir(launcher.Facing());
    // (Straight along its facing; a sideways throw gets a little lift, so it carries.)
    const glm::vec3 velocity = dir * speed + (dir.y == 0.0f ? glm::vec3(0.0f, 5.0f * speed / Power::kLaunchSpeed, 0.0f) : glm::vec3(0.0f));
    const glm::vec3 lo = glm::vec3(pos + launcher.Facing()), hi = lo + glm::vec3(1.0f);
    auto inFront = [&](const glm::vec3& a, const glm::vec3& b) { return glm::all(glm::lessThan(a, hi)) && glm::all(glm::greaterThan(b, lo)); };
    for (auto& [peer, client] : clients) {
        if (!client.welcomed || !client.hasState) continue;
        if (!inFront(client.feet - glm::vec3(0.3f, 0.0f, 0.3f), client.feet + glm::vec3(0.3f, 1.8f, 0.3f))) continue;
        net.Send(peer, net::kChannelReliable, net::Encode(net::Knockback{ velocity }));
        client.knockedUntil = now + 3.0;
        client.launchedUntil = now + 3.0;
        client.fallPeak = std::max(client.fallPeak, client.feet.y);
    }
    std::vector<uint16_t> thrown;
    for (const auto& c : creatures.All())
        if (inFront(c.feet - glm::vec3(0.3f, 0.0f, 0.3f), c.feet + glm::vec3(0.3f, 1.0f, 0.3f))) thrown.push_back(c.id);
    for (const uint16_t id : thrown) creatures.Launch(id, velocity);
    for (auto& d : dropped.All())
        if (inFront(d.position - glm::vec3(DroppedItems::kHalf), d.position + glm::vec3(DroppedItems::kHalf))) d.velocity = velocity, d.onGround = false;
}

void Server::BuildFluidNets() {
    fluidNets.clear();
    fluidNetOf.clear();
    auto blockAt = [&](const glm::ivec3& p) {
        Block b;
        if (!world->GetBlock(p.x, p.y, p.z, b)) b.type = BlockType::COUNT;
        return b;
    };
    auto trace = [&](const glm::ivec3& start) {
        if (fluidNetOf.count(start) || !Fluids::IsPart(blockAt(start).type)) return;
        const int index = static_cast<int>(fluidNets.size());
        fluidNets.emplace_back();
        FluidNet& net = fluidNets.back();
        net.members.push_back(start);
        fluidNetOf[start] = index;
        for (size_t i = 0; i < net.members.size() && net.members.size() < static_cast<size_t>(Power::kMaxNetwork); i++)
            for (const auto& d : kFaces) {
                const glm::ivec3 n = net.members[i] + d;
                if (fluidNetOf.count(n) || !Fluids::IsPart(blockAt(n).type)) continue;
                fluidNetOf[n] = index;
                net.members.push_back(n);
            }
        net.lavaProof = true;
        for (const auto& m : net.members) {
            net.capacity += Fluids::CapacityOf(blockAt(m).type);
            net.lavaProof &= blockAt(m).type == BlockType::OBSIDIAN_PIPE;
            auto h = fluidHeld.find(m);
            if (h == fluidHeld.end() || h->second <= 0) continue;
            net.amount += h->second;
            if (net.kind == Fluids::Kind::None) net.kind = fluidKind[m];
        }
        net.amount = std::min(net.amount, net.capacity);
    };
    for (const auto& pos : machineList) {
        const Block b = blockAt(pos);
        const BlockType t = b.type;
        if (t == BlockType::FLUID_TANK) trace(pos);
        else if (Fluids::IsMachine(t))
            for (const auto& d : kFaces) trace(pos + d);
        else if (t == BlockType::MULTIBLOCK) // (a structure's fluid ports)
            for (const auto role : { Multiblocks::Role::FluidIn, Multiblocks::Role::FluidOut }) {
                glm::ivec3 port;
                if (Multiblocks::PortOf(pos, b, role, port)) trace(port);
            }
    }
    // And pipes holding something with nothing else on them (steam left in a pipe run
    // still lets itself out of its ends).
    for (const auto& [pos, held] : fluidHeld)
        if (held > 0) trace(pos);
}

Server::FluidNet* Server::FluidNetAt(const glm::ivec3& pos) {
    auto it = fluidNetOf.find(pos);
    return it == fluidNetOf.end() ? nullptr : &fluidNets[it->second];
}

Server::FluidNet* Server::FluidNetBeside(const glm::ivec3& pos, Fluids::Kind kind, bool have) {
    for (const auto& d : kFaces) {
        FluidNet* net = FluidNetAt(pos + d);
        if (!net) continue;
        if (have ? (net->kind == kind && net->amount > 0) : (net->Takes(kind) && net->Room() > 0)) return net;
    }
    return nullptr;
}

void Server::StoreFluids() {
    // Parts no longer there lose what they held; networks share theirs out by size.
    for (auto it = fluidHeld.begin(); it != fluidHeld.end();) {
        Block b;
        if (world->GetBlock(it->first.x, it->first.y, it->first.z, b) && !Fluids::IsPart(b.type)) {
            fluidKind.erase(it->first);
            it = fluidHeld.erase(it);
        }
        else ++it;
    }
    for (FluidNet& net : fluidNets) {
        if (net.amount <= 0) net.kind = Fluids::Kind::None, net.amount = 0;
        int left = net.amount;
        for (size_t i = 0; i < net.members.size(); i++) {
            const glm::ivec3& m = net.members[i];
            Block b;
            world->GetBlock(m.x, m.y, m.z, b);
            const int cap = Fluids::CapacityOf(b.type);
            int share = net.capacity > 0 ? static_cast<int>(static_cast<int64_t>(net.amount) * cap / net.capacity) : 0;
            if (i + 1 == net.members.size()) share = left; // (what rounding left over)
            share = std::min(share, std::max(0, left));
            left -= share;
            if (share > 0) {
                fluidHeld[m] = share;
                fluidKind[m] = net.kind;
            }
            else {
                fluidHeld.erase(m);
                fluidKind.erase(m);
            }
        }
    }
}

bool Server::PumpOil(const glm::ivec3& pos, Worker& w) {
    // (A lone derrick block does nothing now: it's built into an oil derrick structure.)
    w.state = Power::DrillState::Blocked;
    return false;
    FluidNet* out = FluidNetBeside(pos, Fluids::Kind::Crude, false);
    if (!out || out->Room() < Fluids::kDerrickUnits) {
        w.state = Power::DrillState::NoChest; // (nowhere to put it)
        return false;
    }
    // The nearest shale under it, in a 3 x 3 column down to its well's depth.
    for (int depth = 1; depth <= Fluids::kWellDepth; depth++)
        for (int dx = -1; dx <= 1; dx++)
            for (int dz = -1; dz <= 1; dz++) {
                const glm::ivec3 p = pos + glm::ivec3(dx, -depth, dz);
                Block b;
                if (!world->GetBlock(p.x, p.y, p.z, b) || b.type != BlockType::OIL_SHALE) continue;
                Block drawn = b;
                if (drawn.axis >= 254) drawn = Block(BlockType::STONE); // spent
                else drawn.axis++;
                world->SetBlock(p.x, p.y, p.z, drawn);
                out->amount += Fluids::kDerrickUnits;
                out->kind = Fluids::Kind::Crude;
                w.state = Power::DrillState::Working;
                return true;
            }
    w.state = Power::DrillState::Done; // (no oil under it)
    return false;
}

bool Server::Refine(const glm::ivec3& pos, const Block& refinery, Worker& w) {
    // (Likewise a lone refinery block: it's built into a refinery structure.)
    w.state = Power::DrillState::Blocked;
    return false;
    FluidNet* in = FluidNetAt(pos - refinery.Facing());
    FluidNet* out = FluidNetAt(pos + refinery.Facing());
    if (!in || !out || in == out) {
        w.state = Power::DrillState::NoChest; // (needs pipes at its back and its front)
        return false;
    }
    if (in->kind != Fluids::Kind::Crude || in->amount <= 0 || !out->Takes(Fluids::Kind::Fuel) || out->Room() <= 0) {
        w.state = Power::DrillState::Idle;
        return false;
    }
    const int units = std::min({ Fluids::kRefineUnits, in->amount, out->Room() });
    in->amount -= units;
    out->amount += units;
    out->kind = Fluids::Kind::Fuel;
    w.state = Power::DrillState::Working;
    return true;
}

bool Server::Movable(const glm::ivec3& pos, const Block& b) const {
    if (b.IsAir() || b.IsLiquid() || b.type == BlockType::COUNT) return false;
    if (signalBlocks.count(pos) || Signals::KindOf(b.type) != Signals::Kind::None) return false;
    switch (b.type) {
    case BlockType::CHEST:
    case BlockType::MINING_POST:
    case BlockType::BED:
    case BlockType::DOOR:
    case BlockType::DOOR_OPEN:
    case BlockType::SIGN:
    case BlockType::TNT_LIT:
    case BlockType::PUMP:
    case BlockType::PIPE:
    case BlockType::SLUICE:
    case BlockType::LADDER:
    case BlockType::CROP:
    case BlockType::RIPE_WHEAT:
    case BlockType::PISTON_HEAD: return false;
    default: return !Power::IsPart(b.type) && !Power::IsMachine(b.type);
    }
}

bool Server::PistonPush(const glm::ivec3& pos, const Block& piston) {
    const glm::ivec3 dir = piston.Facing();
    // The row in front, up to the first open cell (air, water, or a plant to crush).
    std::vector<std::pair<glm::ivec3, Block>> row;
    glm::ivec3 end = pos + dir;
    Block endBlock;
    for (;;) {
        if (!world->GetBlock(end.x, end.y, end.z, endBlock)) return false; // (not loaded)
        if (endBlock.IsAir() || endBlock.IsLiquid() || endBlock.IsReplaceableByWater()) break;
        if (!Movable(end, endBlock) || static_cast<int>(row.size()) >= Power::kPushLimit) return false;
        row.emplace_back(end, endBlock);
        end += dir;
    }
    if (endBlock.IsLiquid() && !PushWaterAside(end)) return false; // (nowhere for the water to go)
    for (int i = static_cast<int>(row.size()) - 1; i >= 0; i--) {
        const glm::ivec3 to = row[i].first + dir;
        world->SetBlock(to.x, to.y, to.z, row[i].second);
    }
    Block head(BlockType::PISTON_HEAD);
    head.axis = piston.axis;
    world->SetBlock(pos.x + dir.x, pos.y + dir.y, pos.z + dir.z, head);
    return true;
}

void Server::PistonPull(const glm::ivec3& pos, const Block& piston) {
    const glm::ivec3 dir = piston.Facing(), head = pos + dir, beyond = pos + dir * 2;
    Block b;
    if (world->GetBlock(beyond.x, beyond.y, beyond.z, b) && Movable(beyond, b) && !b.IsPlant()) {
        world->SetBlock(head.x, head.y, head.z, b);
        world->SetBlock(beyond.x, beyond.y, beyond.z, Block());
    }
    else world->SetBlock(head.x, head.y, head.z, Block());
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
            else if (auto s = smelters.find(pos); s != smelters.end()) {
                // depth: what it made last.
                info.type = static_cast<uint8_t>(BlockType::SMELTER);
                info.state = static_cast<uint8_t>(s->second.state);
                info.depth = static_cast<uint8_t>(s->second.made);
            }
            else if (auto wk = workers.find(pos); wk != workers.end()) {
                Block t;
                world->GetBlock(pos.x, pos.y, pos.z, t);
                info.type = static_cast<uint8_t>(t.type);
                info.state = static_cast<uint8_t>(wk->second.state);
                glm::ivec3 port;
                if (t.type == BlockType::MULTIBLOCK && Multiblocks::PortOf(pos, t, Multiblocks::Role::AirPort, port))
                    if (auto n = networkOf.find(port); n != networkOf.end()) info.pressure = ToByte(networks[n->second].Pressure());
            }
            else if (Block g; world->GetBlock(pos.x, pos.y, pos.z, g) && g.type == BlockType::PRESSURE_GAUGE) {
                // The pressure of what it's fixed to.
                info.type = static_cast<uint8_t>(BlockType::PRESSURE_GAUGE);
                auto on = networkOf.find(pos - g.Facing());
                info.pressure = on != networkOf.end() ? ToByte(networks[on->second].Pressure()) : 0;
                // (On a fluid pipe or tank: how full it is, and of what.)
                if (auto f = fluidNetOf.find(pos - g.Facing()); f != fluidNetOf.end()) {
                    const FluidNet& fn = fluidNets[f->second];
                    info.pressure = fn.capacity > 0 ? ToByte(static_cast<float>(fn.amount) / fn.capacity) : 0;
                    info.depth = static_cast<uint8_t>(fn.kind);
                }
            }
            else if (auto mp = miningPosts.find(pos); mp != miningPosts.end()) {
                // state: working, unpaid (NoPressure) or no ore; depth: how many miners.
                info.type = static_cast<uint8_t>(BlockType::MINING_POST);
                info.state = static_cast<uint8_t>(mp->second.state);
                info.depth = static_cast<uint8_t>(mp->second.miners.size());
            }
            else if (Block ft; world->GetBlock(pos.x, pos.y, pos.z, ft) && ft.type == BlockType::FLUID_TANK) {
                // pressure: how full; depth: of what.
                info.type = static_cast<uint8_t>(BlockType::FLUID_TANK);
                if (const FluidNet* fn = FluidNetAt(pos)) {
                    info.pressure = fn->capacity > 0 ? ToByte(static_cast<float>(fn->amount) / fn->capacity) : 0;
                    info.depth = static_cast<uint8_t>(fn->kind);
                }
            }
            else if (auto df = diffusers.find(pos); df != diffusers.end()) {
                info.type = static_cast<uint8_t>(BlockType::AIR_DIFFUSER);
                info.state = static_cast<uint8_t>(df->second ? Power::DrillState::Working : Power::DrillState::NoPressure);
            }
            else if (auto e = engines.find(pos); e != engines.end()) {
                Block eb;
                world->GetBlock(pos.x, pos.y, pos.z, eb);
                info.type = static_cast<uint8_t>(eb.type == BlockType::TURBINE ? BlockType::TURBINE : BlockType::ENGINE);
                info.state = static_cast<uint8_t>(e->second ? Power::DrillState::Working : Power::DrillState::Idle);
            }
            else if (Block t; world->GetBlock(pos.x, pos.y, pos.z, t) && (t.type == BlockType::AIR_TANK || t.type == BlockType::CREATIVE_AIR_TANK)) {
                info.type = static_cast<uint8_t>(t.type);
            }
            else if (Block l; world->GetBlock(pos.x, pos.y, pos.z, l) && l.type == BlockType::LAUNCHER) {
                info.type = static_cast<uint8_t>(BlockType::LAUNCHER);
                auto f = launcherFired.find(pos);
                info.state = static_cast<uint8_t>(f != launcherFired.end() && Now() - f->second < 0.6 ? Power::DrillState::Working : Power::DrillState::Idle);
                auto on = networkOf.find(pos);
                info.pressure = on != networkOf.end() ? ToByte(networks[on->second].Pressure()) : 0;
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
    auto describeFluid = [&](const FluidNet* net) {
        if (!net) return std::string("Not joined to any fluid pipes.");
        std::snprintf(line, sizeof line, "Its pipes hold %d of %d units%s%s.", net->amount, net->capacity, net->amount > 0 ? " of " : "",
                      net->amount > 0 ? Fluids::Name(net->kind) : " (empty)");
        return std::string(line);
    };
    if (b.type == BlockType::MULTIBLOCK || b.type == BlockType::MULTIBLOCK_PART) {
        glm::ivec3 core;
        Block cb;
        if (!CoreOf(pos, core) || !world->GetBlock(core.x, core.y, core.z, cb)) return;
        const Multiblocks::Pattern& p = Multiblocks::PatternOf(Multiblocks::KindOf(cb));
        const Worker w = workers.count(core) ? workers[core] : Worker{};
        text = std::string(p.name) + " (a structure): ";
        switch (w.state) {
        case Power::DrillState::Working: text += "working. "; break;
        case Power::DrillState::NoPressure: text += "waiting for air pressure (at its air pipe). "; break;
        case Power::DrillState::NoChest: text += "its ports need connecting (pipes, or chests along its item pipe). "; break;
        case Power::DrillState::Done: text += "no oil shale under it. "; break;
        default: text += Multiblocks::KindOf(cb) == Multiblocks::Kind::BlastFurnace ? "nothing to smelt. " : "idle (nothing in, or no room out). ";
        }
        glm::ivec3 port;
        if (Multiblocks::PortOf(core, cb, Multiblocks::Role::AirPort, port)) {
            auto n = networkOf.find(port);
            std::snprintf(line, sizeof line, "Air pressure %d%%.", n != networkOf.end() ? int(std::lround(networks[n->second].Pressure() * 100)) : 0);
            text += line;
        }
        return SendChat(client, net::ChatMessage::Kind::Inspect, text);
    }
    if (Fluids::IsPart(b.type)) {
        text = std::string(b.type == BlockType::FLUID_TANK ? "Fluid tank. " : "Fluid pipe. ") + describeFluid(FluidNetAt(pos));
        return SendChat(client, net::ChatMessage::Kind::Inspect, text);
    }
    if (b.type == BlockType::DRAIN || b.type == BlockType::FLUID_OUTLET) {
        const FluidNet* net = nullptr;
        for (const auto& d : kFaces)
            if ((net = FluidNetAt(pos + d))) break;
        text = b.type == BlockType::DRAIN ? "Drain: takes water into the fluid pipes beside it, as much as they have room for. "
                                          : "Fluid outlet: lets water from the fluid pipes beside it out of its face. ";
        return SendChat(client, net::ChatMessage::Kind::Inspect, text + describeFluid(net));
    }
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
        else if (b.type == BlockType::CREATIVE_AIR_TANK) {
            text += "Creative air tank: keeps this network full, however much air it uses. ";
        }
        else if (b.type == BlockType::FLUID_VALVE || b.type == BlockType::FLUID_VALVE_OPEN) {
            return SendChat(client, net::ChatMessage::Kind::Inspect,
                            b.type == BlockType::FLUID_VALVE_OPEN ? "Fluid valve: open." : "Fluid valve: shut (the fluid pipes either side are kept apart).");
        }
        else if (b.type == BlockType::VALVE || b.type == BlockType::VALVE_OPEN) {
            text += b.type == BlockType::VALVE_OPEN ? "Valve: open. " : "Valve: shut (the pipes either side are kept apart). ";
        }
        else if (b.type == BlockType::LAUNCHER) {
            auto sig = signalBlocks.find(pos);
            text += std::string("Air launcher: throws what's in front of it each time its channel comes on (") +
                    (sig != signalBlocks.end() && sig->second.config.a ? "channel " + std::to_string(sig->second.config.a) : std::string("no channel yet")) + "), at " +
                    std::to_string(static_cast<int>(Power::LaunchSpeed(sig != signalBlocks.end() ? sig->second.config.b : 0))) + " blocks a second. ";
        }
        else if (b.type == BlockType::PISTON) {
            const Power::DrillState st = pistonStates.count(pos) ? pistonStates[pos] : Power::DrillState::Idle;
            auto sig = signalBlocks.find(pos);
            text += st == Power::DrillState::Working ? "Piston: pushed out. " : st == Power::DrillState::NoPressure ? "Piston: waiting for air pressure. "
                  : st == Power::DrillState::Blocked ? "Piston: blocked (something it can't move, or too many blocks in a row). " : "Piston: in. ";
            text += sig != signalBlocks.end() && sig->second.config.a ? "Listens to channel " + std::to_string(sig->second.config.a) + ". "
                                                                      : "No channel (crouch and right-click to set one). ";
        }
        else if (b.type == BlockType::SMELTER || b.type == BlockType::SMELTER_LIT) {
            const Smelter sm = smelters.count(pos) ? smelters[pos] : Smelter{};
            const size_t chestCount = ChestsJoined(pos).size();
            text += sm.state == Power::DrillState::Working ? "Smelter: smelting. " : sm.state == Power::DrillState::NoPressure ? "Smelter: waiting for air pressure. "
                  : sm.state == Power::DrillState::NoChest ? "Smelter: needs a chest touching it or joined by item pipes. " : "Smelter: nothing to smelt in its chests. ";
            text += chestCount > 1 ? std::to_string(chestCount - 1) + (chestCount == 2 ? " chest joined, " : " chests joined, ") + "besides its own inventory. "
                                   : "Working from its own inventory (right-click it empty-handed). ";
        }
        else if (b.type == BlockType::CRUSHER) {
            const Worker w = workers.count(pos) ? workers[pos] : Worker{};
            text += w.state == Power::DrillState::Working      ? "Crusher: grinding. "
                    : w.state == Power::DrillState::NoPressure ? "Crusher: waiting for air pressure. "
                    : w.state == Power::DrillState::NoChest    ? "Crusher: needs a chest touching it or joined by item pipes. "
                                                               : "Crusher: no iron ore or stone in its chests. ";
        }
        else if (b.type == BlockType::EXTRACTOR || b.type == BlockType::CRAFTER || b.type == BlockType::HARVESTER) {
            const Worker w = workers.count(pos) ? workers[pos] : Worker{};
            const char* name = b.type == BlockType::EXTRACTOR ? "Extractor" : b.type == BlockType::CRAFTER ? "Crafter" : "Harvester";
            text += std::string(name) + ": ";
            switch (w.state) {
            case Power::DrillState::Working: text += "working. "; break;
            case Power::DrillState::NoPressure: text += "waiting for air pressure. "; break;
            case Power::DrillState::NoChest:
                text += b.type == BlockType::EXTRACTOR ? "needs a chest in front of it. " : "needs a chest touching it or joined by item pipes. ";
                break;
            case Power::DrillState::Blocked: text += "no recipe (right-click it with what it should make in hand). "; break;
            default:
                text += b.type == BlockType::EXTRACTOR ? "nothing it can send anywhere. " : b.type == BlockType::CRAFTER ? "its chests don't have what it takes. " : "no ripe wheat near. ";
            }
            if (b.type == BlockType::CRAFTER) {
                const int index = static_cast<int>(b.axis) - 1;
                if (index >= 0 && index < static_cast<int>(Crafting::Recipes().size())) text += std::string("Makes: ") + Crafting::Recipes()[index].name + ". ";
            }
        }
        else if (b.type == BlockType::PRESSURE_SENSOR) {
            text += "Pressure sensor. ";
        }
        else if (b.type == BlockType::PRESSURE_GAUGE) {
            text += "Pressure gauge. ";
            if (const FluidNet* fn = FluidNetAt(pos - b.Facing())) return SendChat(client, net::ChatMessage::Kind::Inspect, text + describeFluid(fn));
        }
        else if (b.type == BlockType::OIL_DERRICK || b.type == BlockType::REFINERY) {
            text += b.type == BlockType::OIL_DERRICK ? "Oil derrick: build the derrick structure round it (steel frames), then strike it with an engineer's hammer. "
                                                     : "Refinery: build the refinery structure round it (machine casings and fluid tanks), then strike it with an engineer's hammer. ";
            return SendChat(client, net::ChatMessage::Kind::Inspect, text);
        }
        else if (false) {
            const Worker w = workers.count(pos) ? workers[pos] : Worker{};
            if (b.type == BlockType::OIL_DERRICK)
                text += w.state == Power::DrillState::Working ? "Oil derrick: pumping crude oil. " : w.state == Power::DrillState::NoPressure ? "Oil derrick: waiting for air pressure. "
                      : w.state == Power::DrillState::NoChest ? "Oil derrick: needs fluid pipes beside it with room for crude oil. "
                      : w.state == Power::DrillState::Done ? "Oil derrick: no oil shale under it. " : "Oil derrick: idle. ";
            else
                text += w.state == Power::DrillState::Working ? "Refinery: making fuel. " : w.state == Power::DrillState::NoPressure ? "Refinery: waiting for air pressure. "
                      : w.state == Power::DrillState::NoChest ? "Refinery: needs fluid pipes at its back (crude in) and in front (fuel out). "
                      : "Refinery: no crude oil behind it, or no room for fuel in front. ";
        }
        else if (b.type == BlockType::AIR_DIFFUSER) {
            text += diffusers.count(pos) && diffusers[pos] ? "Air diffuser: breathing out fresh air (8 blocks round). " : "Air diffuser: stopped (it needs air pressure). ";
        }
        else if (b.type == BlockType::TURBINE) {
            text += engines.count(pos) && engines[pos] ? "Steam turbine: running. " : "Steam turbine: stopped (it needs fluid pipes beside it with steam). ";
        }
        else if (b.type == BlockType::OBSIDIAN_PIPE && !FluidNetAt(pos)) {
            text += "Obsidian pipe: empty. Dip an end in lava and it draws it in (only obsidian pipes hold lava). ";
        }
        else if (b.type == BlockType::BOILER) {
            const Worker w = workers.count(pos) ? workers[pos] : Worker{};
            text += w.state == Power::DrillState::Working      ? "Boiler: boiling. "
                    : w.state == Power::DrillState::NoPressure ? "Boiler: out of fuel (coal or coke in a chest joined to it). "
                    : w.state == Power::DrillState::NoChest    ? "Boiler: needs fluid pipes under it (water in) and over it (steam out). "
                                                               : "Boiler: no water under it, or no room for steam over it. ";
        }
        else if (b.type == BlockType::ENGINE) {
            text += engines.count(pos) && engines[pos] ? "Engine: running (burning fuel). " : "Engine: stopped (it needs fluid pipes beside it with fuel). ";
        }
        else if (b.type == BlockType::PUMP) {
            const Power::DrillState st = pumpStates.count(pos) ? pumpStates[pos] : Power::DrillState::Idle;
            text += st == Power::DrillState::Working ? "Pump: pumping. " : st == Power::DrillState::NoPressure ? "Pump: waiting for air pressure. " : "Pump: nothing to pump. ";
        }
        // (A gauge reads what it's fixed to.)
        auto n = networkOf.find(b.type == BlockType::PRESSURE_GAUGE ? pos - b.Facing() : pos);
        if (n == networkOf.end()) text += b.type == BlockType::PRESSURE_GAUGE ? "It isn't fixed to an air pipe (or anything joined to a compressor)." : "Not joined to a compressor or machine.";
        else {
            const AirNetwork& net = networks[n->second];
            std::snprintf(line, sizeof line, "Air pressure %d%% (%.0f of %.0f air; %d pipes, %d tanks; compressors: %d, +%.1f air/s; drills working: %d of %d; pumps working: %d of %d)",
                          int(std::lround(net.Pressure() * 100)), net.air, net.capacity, net.pipes, net.tanks, net.compressors, net.supply, net.working, net.drills, net.pumping, net.pumps);
            text += line;
        }
    }
    SendChat(client, net::ChatMessage::Kind::Inspect, text);
}
