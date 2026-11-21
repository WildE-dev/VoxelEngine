#pragma once
// Hunger (survival). Players have kMaxFood points of food (drawn as drumsticks, two
// points each). Food drains slowly with time and faster with walking, and healing
// costs food: health only comes back while food is above kRegenFood. At no food at
// all you starve, losing health down to the last heart. Eating raises it: venison
// from deer (cooked is far better). The server keeps the count (Server::UpdateHunger)
// and handles eating (Server::HandleEat). Creative players don't get hungry.
#include <cstdint>

#include "world/Block.h"

namespace Hunger {

constexpr int kMaxFood = 20;
constexpr int kRegenFood = 14;          // health regenerates only above this
// Food lost: one point per this many seconds, plus one per this many blocks walked,
// plus this much for each half heart healed.
constexpr float kSecondsPerPoint = 50.0f;
constexpr float kBlocksPerPoint = 60.0f;
constexpr float kPointsPerHealing = 0.5f;
// Starving (no food): a half heart every this many seconds, down to kStarveFloor.
constexpr double kStarveInterval = 4.0;
constexpr int kStarveFloor = 2;

// How much a whole item of food gives (0: not food).
inline int FoodValue(BlockType type) {
    switch (type) {
    case BlockType::RAW_VENISON: return 3;
    case BlockType::COOKED_VENISON: return 8;
    default: return 0;
    }
}
inline bool IsFood(BlockType type) { return FoodValue(type) > 0; }

// Food left after `exhaustion` more points of drain, carried in `exhaustion` as the
// fraction not yet taken.
inline int Drain(int food, float& exhaustion) {
    while (exhaustion >= 1.0f && food > 0) {
        exhaustion -= 1.0f;
        food--;
    }
    if (food == 0) exhaustion = 0.0f; // (nothing left to take)
    return food;
}

} // namespace Hunger
