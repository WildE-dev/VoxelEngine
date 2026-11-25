// Multiblock structures: forming, taking apart, and running them (see
// world/Multiblocks.h).
#include <algorithm>
#include <cstring>

#include "server/Server.h"
#include "server/ServerWorld.h"
#include "world/Multiblocks.h"
#include "world/Smelting.h"

using namespace Multiblocks;

void Server::TryForm(ClientState* client, const glm::ivec3& pos) {
    auto article = [](const char* name) { return std::string(std::strchr("AEIOUaeiou", name[0]) ? "an " : "a ") + name; };
    auto say = [&](const std::string& text) {
        if (client) SendChat(*client, net::ChatMessage::Kind::Info, text);
        else Log(text);
    };
    Block hit;
    if (!world->GetBlock(pos.x, pos.y, pos.z, hit)) return; // (in reach: the caller checked)
    if (hit.type == BlockType::MULTIBLOCK || hit.type == BlockType::MULTIBLOCK_PART)
        return say("That's already a formed structure");
    // Every way it could be part of a pattern: each kind, each turn, each cell of that
    // block type. The best near miss is what's reported. Blocks in the cells inside it
    // that must be left empty count against it too (`extra`).
    struct Try {
        const Pattern* pattern = nullptr;
        int turn = 0, wrong = INT_MAX, extra = 0;
        glm::ivec3 origin{ 0 }, firstWrong{ 0 }, firstExtra{ 0 };
        BlockType wanted = BlockType::AIR;
    } best;
    for (int k = 0; k < static_cast<int>(Kind::Count); k++) {
        const Pattern& p = PatternOf(static_cast<Kind>(k));
        for (int turn = 0; turn < 4; turn++)
            for (const auto& anchor : p.cells) {
                if (anchor.type != hit.type) continue;
                const glm::ivec3 origin = pos - Place(glm::ivec3(0), turn, anchor.at);
                Try t{ &p, turn, 0, 0, origin, {}, {}, BlockType::AIR };
                for (const auto& c : p.cells) {
                    const glm::ivec3 at = Place(origin, turn, c.at);
                    Block b;
                    if (world->GetBlock(at.x, at.y, at.z, b) && b.type == c.type) continue;
                    if (t.wrong++ == 0) t.firstWrong = at, t.wanted = c.type;
                }
                for (const auto& o : p.open) {
                    const glm::ivec3 at = Place(origin, turn, o);
                    Block b;
                    if (!world->GetBlock(at.x, at.y, at.z, b) || b.IsAir() || b.IsLiquid()) continue;
                    if (t.extra++ == 0) t.firstExtra = at;
                }
                if (t.wrong + t.extra < best.wrong + best.extra) best = t;
            }
    }
    if (!best.pattern) return say("That isn't part of any structure (strike a block of one with the hammer)");
    if (best.wrong > 0) {
        char line[200];
        std::snprintf(line, sizeof line, "Not quite %s: %d block%s missing or wrong (at %d %d %d it needs %s)", article(best.pattern->name).c_str(), best.wrong, best.wrong == 1 ? "" : "s",
                      best.firstWrong.x, best.firstWrong.y, best.firstWrong.z, BlockId(best.wanted));
        return say(line);
    }
    if (best.extra > 0) {
        char line[200];
        std::snprintf(line, sizeof line, "Not quite %s: %d block%s in the way inside it (at %d %d %d), which must be left empty", article(best.pattern->name).c_str(), best.extra,
                      best.extra == 1 ? "" : "s", best.firstExtra.x, best.firstExtra.y, best.firstExtra.z);
        return say(line);
    }
    // Complete: the core and parts become the structure (ports stay as they are).
    for (const auto& c : best.pattern->cells) {
        const glm::ivec3 at = Place(best.origin, best.turn, c.at);
        if (c.role == Role::Part) world->SetBlock(at.x, at.y, at.z, Block(BlockType::MULTIBLOCK_PART));
        else if (c.role == Role::Core) {
            Block core(BlockType::MULTIBLOCK);
            core.axis = Encode(best.pattern->kind, best.turn);
            world->SetBlock(at.x, at.y, at.z, core);
        }
    }
    say("Formed " + article(best.pattern->name));
}

bool Server::CoreOf(const glm::ivec3& pos, glm::ivec3& core) const {
    Block b;
    if (!world->GetBlock(pos.x, pos.y, pos.z, b)) return false;
    if (b.type == BlockType::MULTIBLOCK) {
        core = pos;
        return true;
    }
    // A part: the core whose pattern covers it (structures are 3 across and a few high).
    for (int dx = -2; dx <= 2; dx++)
        for (int dz = -2; dz <= 2; dz++)
            for (int dy = -5; dy <= 5; dy++) {
                const glm::ivec3 p = pos + glm::ivec3(dx, dy, dz);
                Block c;
                if (!world->GetBlock(p.x, p.y, p.z, c) || c.type != BlockType::MULTIBLOCK) continue;
                const glm::ivec3 origin = OriginOf(p, c);
                for (const auto& cell : PatternOf(KindOf(c)).cells)
                    if (cell.role == Role::Part && Place(origin, TurnOf(c), cell.at) == pos) {
                        core = p;
                        return true;
                    }
            }
    return false;
}

bool Server::StructureWhole(const glm::ivec3& core, const Block& block) const {
    const glm::ivec3 origin = OriginOf(core, block);
    for (const auto& c : PatternOf(KindOf(block)).cells) {
        if (c.role != Role::Part) continue;
        const glm::ivec3 at = Place(origin, TurnOf(block), c.at);
        Block b;
        if (world->GetBlock(at.x, at.y, at.z, b) && b.type != BlockType::MULTIBLOCK_PART) return false; // (unloaded: assume so)
    }
    return true;
}

Block Server::TakeApart(const glm::ivec3& pos) {
    glm::ivec3 core;
    Block was;
    if (!world->GetBlock(pos.x, pos.y, pos.z, was)) return was;
    Block coreBlock;
    if (!CoreOf(pos, core) || !world->GetBlock(core.x, core.y, core.z, coreBlock)) return Block(); // (a stray part: nothing)
    const glm::ivec3 origin = OriginOf(core, coreBlock);
    Block original;
    for (const auto& c : PatternOf(KindOf(coreBlock)).cells) {
        if (c.role != Role::Part && c.role != Role::Core) continue;
        const glm::ivec3 at = Place(origin, TurnOf(coreBlock), c.at);
        Block b;
        if (!world->GetBlock(at.x, at.y, at.z, b) || (b.type != BlockType::MULTIBLOCK && b.type != BlockType::MULTIBLOCK_PART)) continue;
        world->SetBlock(at.x, at.y, at.z, Block(c.type));
        if (at == pos) original = Block(c.type);
    }
    workers.erase(core);
    return original;
}

bool Server::RunStructure(const glm::ivec3& core, const Block& block, Worker& w) {
    const Kind kind = KindOf(block);
    const int turn = TurnOf(block);
    glm::ivec3 port;
    switch (kind) {
    case Kind::OilDerrick: {
        if (!PortOf(core, block, Role::FluidOut, port)) return false;
        FluidNet* out = FluidNetAt(port);
        if (!out || !out->Takes(Fluids::Kind::Crude) || out->Room() < kStructureCrude) {
            w.state = Power::DrillState::NoChest;
            return false;
        }
        // The nearest shale under its footprint, down to its well's depth.
        const glm::ivec3 origin = OriginOf(core, block);
        for (int depth = 1; depth <= kStructureWellDepth; depth++)
            for (int x = 0; x < 3; x++)
                for (int z = 0; z < 3; z++) {
                    const glm::ivec3 p = Place(origin, turn, glm::ivec3(x, 0, z)) - glm::ivec3(0, depth, 0);
                    Block b;
                    if (!world->GetBlock(p.x, p.y, p.z, b) || b.type != BlockType::OIL_SHALE) continue;
                    // Three draws' worth at once (the shale counts them).
                    Block drawn = b;
                    const int draws = kStructureCrude / Fluids::kDerrickUnits;
                    if (drawn.axis + draws >= 255) drawn = Block(BlockType::STONE);
                    else drawn.axis = static_cast<uint8_t>(drawn.axis + draws);
                    world->SetBlock(p.x, p.y, p.z, drawn);
                    out->amount += kStructureCrude;
                    out->kind = Fluids::Kind::Crude;
                    w.state = Power::DrillState::Working;
                    return true;
                }
        w.state = Power::DrillState::Done;
        return false;
    }
    case Kind::Refinery: {
        glm::ivec3 inPort, outPort;
        if (!PortOf(core, block, Role::FluidIn, inPort) || !PortOf(core, block, Role::FluidOut, outPort)) return false;
        FluidNet* in = FluidNetAt(inPort);
        FluidNet* out = FluidNetAt(outPort);
        if (!in || !out || in == out) {
            w.state = Power::DrillState::NoChest;
            return false;
        }
        if (in->kind != Fluids::Kind::Crude || in->amount <= 0 || !out->Takes(Fluids::Kind::Fuel) || out->Room() <= 0) {
            w.state = Power::DrillState::Idle;
            return false;
        }
        const int units = std::min({ kStructureRefine, in->amount, out->Room() });
        in->amount -= units;
        out->amount += units;
        out->kind = Fluids::Kind::Fuel;
        // What's left over: bitumen, into its own inventory (while there's room for it).
        int& toward = refinedTowardBitumen[core];
        toward += units;
        if (toward >= kCrudePerBitumen) {
            toward -= kCrudePerBitumen;
            if (ChestAt(core).AddUnits(ItemType::BITUMEN, Block::kUnitsPerBlock) == 0) SendChestToViewers(core);
        }
        w.state = Power::DrillState::Working;
        return true;
    }
    case Kind::BlastFurnace: {
        if (!PortOf(core, block, Role::ItemPort, port)) return false;
        std::vector<glm::ivec3> joined = ChestsJoined(port);
        joined.insert(joined.begin(), core); // (its own inventory first)
        if (joined.empty()) {
            w.state = Power::DrillState::NoChest;
            return false;
        }
        for (const auto& c : joined) {
            Inventory& chest = ChestAt(c);
            const SmeltRecipe* r = Smelting::FirstAffordable(chest);
            if (!r || !Smelting::Apply(*r, chest)) continue;
            SendChestToViewers(c);
            w.state = Power::DrillState::Working;
            return true;
        }
        w.state = Power::DrillState::Idle;
        return false;
    }
    case Kind::CokeOven: {
        if (!PortOf(core, block, Role::ItemPort, port)) return false;
        std::vector<glm::ivec3> joined = ChestsJoined(port);
        joined.insert(joined.begin(), core); // (its own inventory first)
        if (joined.empty()) {
            w.state = Power::DrillState::NoChest;
            return false;
        }
        for (const auto& c : joined) {
            Inventory& chest = ChestAt(c);
            if (chest.Units(ItemType::COAL) < 2 * Block::kUnitsPerBlock) continue;
            Inventory after = chest;
            after.TakeUnits(ItemType::COAL, 2 * Block::kUnitsPerBlock);
            if (after.AddUnits(ItemType::COKE, Block::kUnitsPerBlock) != 0) continue; // (full)
            chest = std::move(after);
            SendChestToViewers(c);
            w.state = Power::DrillState::Working;
            return true;
        }
        w.state = Power::DrillState::Idle;
        return false;
    }
    default: return false;
    }
}
