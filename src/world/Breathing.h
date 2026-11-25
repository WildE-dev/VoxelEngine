#pragma once
// Breathing (survival): each player has an air meter, kMaxAir units (shown as ten
// bubbles). It drains while their head is under water, and fills back up otherwise.
// Thin air far up (above 300) and stale air deep down (below -100) don't drain it:
// they cap it, lower the further in you are, and it settles at the cap. Empty, it
// hurts: drowning quickly, thin or stale air slowly. The server keeps the
// meter (see Server::UpdateBreath); the client uses the same rules to show why.
// An air diffuser (a machine on an air network, world/Power.h) breathes out fresh
// air: within kDiffuserRange of a working one, thin and stale air don't count (water
// still does: it's air for the lungs, not gills).
// An oxygen tank (worn, world/Armour.h) holds kTankSeconds of air: while anything
// would take your breath (water too), you breathe from it instead, and it empties;
// wherever the air's good (the surface, or a diffuser's reach) it fills back up at
// kTankRefill seconds of air a second.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "world/Block.h"

namespace Breathing {

constexpr float kMaxAir = 20.0f;              // units; a bubble is two
constexpr float kDrownSeconds = 15.0f;        // a full breath under water
constexpr float kRecoverPerSecond = 4.0f;     // back to full in five seconds
// Thin air above this height (eye level, far above the highest peaks, so only a
// tower takes you there), and stale air below this depth (well under the dungeons).
// The air you can hold falls in proportion to how far in you are, to nothing
// kAirSpan blocks in: half at 350 or -150, none at 400 or -200.
constexpr float kThinAirFrom = 300.0f;
constexpr float kStaleAirBelow = -100.0f;
constexpr float kAirSpan = 100.0f;
// Below this much air, thin or stale air leaves you short of breath: no sprinting.
constexpr float kShortOfBreath = kMaxAir * 0.5f;

// Damage once the air has run out (half hearts, every interval).
constexpr int kDrownDamage = 2;
constexpr double kDrownInterval = 1.0;
constexpr int kSuffocateDamage = 1;
constexpr double kSuffocateInterval = 1.5;

// An air diffuser: how far its fresh air reaches (from its middle to your eyes), and
// the compressed air it uses a second while it runs.
constexpr float kDiffuserRange = 8.0f;
constexpr float kDiffuserAir = 1.5f;
constexpr float kTankSeconds = 90.0f;
constexpr float kTankRefill = 6.0f;

enum class Cause : uint8_t { None, Water, ThinAir, StaleAir };

inline const char* CauseName(Cause c) {
    switch (c) {
    case Cause::Water: return "Under water";
    case Cause::ThinAir: return "Thin air";
    case Cause::StaleAir: return "Stale air";
    default: return "";
    }
}

struct Air {
    Cause cause = Cause::None;
    float rate = kRecoverPerSecond; // air units per second: draining under water, else refilling
    float limit = kMaxAir;          // the most air there is to breathe here (thin or stale air)
};

// What breathing is like with the eyes at height eyeY, under water or not (and with
// fresh air from a diffuser, or not).
inline Air At(float eyeY, bool headInWater, bool freshAir = false) {
    Air air;
    const float thin = eyeY - kThinAirFrom, stale = kStaleAirBelow - eyeY;
    if ((thin > 0.0f || stale > 0.0f) && !freshAir) {
        air.cause = thin > 0.0f ? Cause::ThinAir : Cause::StaleAir;
        air.limit = kMaxAir * std::max(0.0f, 1.0f - std::max(thin, stale) / kAirSpan);
    }
    if (headInWater) {
        air.cause = Cause::Water;
        air.rate = -kMaxAir / kDrownSeconds;
    }
    return air;
}

// A step of the air meter: under water it drains; otherwise it moves to the limit,
// filling up (or, coming into thinner air, dropping) at kRecoverPerSecond.
inline float Step(float current, const Air& air, float dt) {
    if (air.rate < 0.0f) return std::clamp(std::min(current, air.limit) + air.rate * dt, 0.0f, kMaxAir);
    const float step = kRecoverPerSecond * dt;
    return current < air.limit ? std::min(air.limit, current + step) : std::max(air.limit, current - step);
}

// Whether the air left is enough to breathe: out of it under water, or where
// there's none (an empty limit), breathing hurts.
inline bool Suffocating(float current, const Air& air) {
    return current <= 0.0f && (air.cause == Cause::Water || air.limit <= 0.0f);
}

// Whether a point is under the water in a cell (its block, and the point's height
// within the cell, 0..1).
inline bool UnderWater(const Block& block, float heightInCell) {
    return block.HasWater() && heightInCell < block.WaterLevel() / static_cast<float>(Block::kMaxWaterVolume);
}

} // namespace Breathing
