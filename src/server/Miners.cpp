// Mining posts and the dwarf miners hired at them (see Server.h).
#include <algorithm>
#include <chrono>

#include "server/Server.h"
#include "server/ServerWorld.h"

namespace {

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// What miners are paid in: cooked food, the cheapest first.
const BlockType kPay[] = { BlockType::BREAD, BlockType::COOKED_FISH, BlockType::COOKED_VENISON };

bool IsOre(BlockType t) { return t == BlockType::COAL_ORE || t == BlockType::IRON_ORE; }

} // namespace

void Server::UpdateMiningPosts() {
    const double now = Now();
    if (now < nextMinersUpdate) return;
    nextMinersUpdate = now + 0.5;
    std::unordered_set<glm::ivec3, Coords::IVec3Hash> present;
    for (const auto& pos : machineList) {
        Block b;
        if (!world->GetBlock(pos.x, pos.y, pos.z, b) || b.type != BlockType::MINING_POST) continue;
        present.insert(pos);
        MiningPost& post = miningPosts[pos];
        // Miners who've gone (killed, or wandered off out of everyone's sight).
        for (size_t i = 0; i < post.miners.size();)
            if (!creatures.Find(post.miners[i])) {
                post.miners.erase(post.miners.begin() + static_cast<long>(i));
                post.nextDig.erase(post.nextDig.begin() + static_cast<long>(i));
            }
            else i++;

        ChestItems& held = ChestAt(pos);
        uint32_t food = 0;
        for (BlockType t : kPay) food += held[static_cast<size_t>(t)];
        const bool paid = food >= kMinerPay;
        if (!paid) {
            if (post.unpaidSince < 0.0) post.unpaidSince = now;
            post.state = Power::DrillState::NoPressure;
            if (now - post.unpaidSince > kUnpaidLeave) {
                for (uint16_t id : post.miners) creatures.Remove(id);
                post.miners.clear();
                post.nextDig.clear();
            }
            continue;
        }
        post.unpaidSince = -1.0;

        // Paid: another miner turns up now and then, standing beside the post.
        if (static_cast<int>(post.miners.size()) < kMaxMiners && now >= post.nextArrival) {
            post.nextArrival = now + 4.0;
            static const glm::ivec3 kAround[8] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 } };
            for (const auto& d : kAround) {
                const glm::ivec3 p = pos + d;
                Block at, head, below;
                if (!world->GetBlock(p.x, p.y, p.z, at) || !world->GetBlock(p.x, p.y + 1, p.z, head) || !world->GetBlock(p.x, p.y - 1, p.z, below)) continue;
                if (!at.IsPassable() || !head.IsPassable() || below.IsPassable()) continue;
                if (const uint16_t id = creatures.SpawnMiner(glm::vec3(p) + glm::vec3(0.5f, 0.0f, 0.5f), pos)) {
                    post.miners.push_back(id);
                    post.nextDig.push_back(now + kMinerInterval);
                }
                break;
            }
        }

        // Each miner, in turn: the nearest ore in reach, dug out and put in the post.
        for (size_t i = 0; i < post.miners.size(); i++) {
            if (now < post.nextDig[i]) continue;
            post.nextDig[i] = now + kMinerInterval;
            std::optional<glm::ivec3> best;
            int bestDistance = INT_MAX;
            const int r = kPostRadius;
            for (int dx = -r; dx <= r; dx++)
                for (int dy = -r; dy <= r; dy++)
                    for (int dz = -r; dz <= r; dz++) {
                        const int d2 = dx * dx + dy * dy + dz * dz;
                        if (d2 > r * r || d2 >= bestDistance) continue;
                        Block ore;
                        const glm::ivec3 p = pos + glm::ivec3(dx, dy, dz);
                        if (world->GetBlock(p.x, p.y, p.z, ore) && IsOre(ore.type)) best = p, bestDistance = d2;
                    }
            if (!best) {
                post.state = Power::DrillState::Done;
                continue;
            }
            Block ore;
            world->GetBlock(best->x, best->y, best->z, ore);
            world->SetBlock(best->x, best->y, best->z, Block());
            held[static_cast<size_t>(ore.ResourceType())] += static_cast<uint32_t>(ore.ResourceUnits());
            uint32_t owed = kMinerPay;
            for (BlockType t : kPay) {
                uint32_t& f = held[static_cast<size_t>(t)];
                const uint32_t take = std::min(owed, f);
                f -= take;
                owed -= take;
            }
            SendChestToViewers(pos);
            post.state = Power::DrillState::Working;
        }
    }
    // Posts that are gone: their miners leave.
    for (auto it = miningPosts.begin(); it != miningPosts.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        Block b;
        if (world->GetBlock(it->first.x, it->first.y, it->first.z, b) && b.type != BlockType::MINING_POST) {
            for (uint16_t id : it->second.miners) creatures.Remove(id);
            it = miningPosts.erase(it);
        }
        else ++it;
    }
}
