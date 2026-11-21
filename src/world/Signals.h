#pragma once
// Signals: wireless, on numbered channels (1..255; 0 is none). Anything sending on a
// channel is heard by everything listening to that channel within kRange blocks.
//
// Senders:
//  - a lever, while it's on (right-click flips it);
//  - a pressure plate, while it's stood on and for kPlateSeconds after;
//  - a logic gate, while its output is on;
//  - sensors: a water sensor while any of the six cells touching it holds water (an
//    eighth of a block or more); a pressure sensor (part of an air network) while the
//    network's pressure is at least its setting, b percent (any air at all, with 0); a
//    daylight sensor by day, while the sky is open above it; a player sensor while a
//    player is within b blocks of it (kPlayerRange, with 0; at most kMaxPlayerRange).
// Listeners:
//  - a signal lamp lights while its channel is on;
//  - a valve opens while its channel is on (a valve with no channel is opened and
//    shut by hand);
//  - a sluice gate with a channel opens (out of the way, letting water through) while
//    its channel is on and closes again when it's off, pushing any water in its cell
//    up or aside (it waits if there's nowhere for the water to go). Without a channel
//    it stays shut. (A dungeon flood chamber's plate sets a latch that opens its gates,
//    on channels of their own: Dungeons::FloodChamberSignals.)
//  - a piston pushes out while its channel is on, and pulls back when it's off (it
//    runs on air: see world/Power.h);
//  - a logic gate's inputs.
// Logic gates work out their output from what they hear on their inputs (a and b),
// each update, and send it on their output channel from the next: AND, OR, XOR, NOT
// (of a), RELAY (a, sent on again: to carry a signal further than kRange), and CLOCK
// (on and off every kClockSeconds while a is on, or always with no a), and LATCH (a
// sets it, b resets it: it turns on when a comes on and stays on, after a goes off,
// until b comes on; while b is on it's off whatever a does).
// The server runs it (Server::UpdateSignals, every kInterval seconds), keeps every
// block's channels, and saves them. Crouch-right-click (or right-click, for those with
// nothing else to do) opens a window to set them.
#include <cstdint>

#include "world/Block.h"

namespace Signals {

constexpr double kInterval = 0.1;
constexpr float kRange = 64.0f;
constexpr double kPlateSeconds = 1.0;
constexpr double kClockSeconds = 1.0;
constexpr int kPlayerRange = 4;
constexpr int kMaxPlayerRange = 16;

enum class Kind : uint8_t { None, Lever, Plate, Gate, Lamp, Valve, Sluice, Piston, WaterSensor, PressureSensor, DaylightSensor, PlayerSensor, Count };
enum class Mode : uint8_t { And, Or, Xor, Not, Relay, Clock, Latch, Count };

inline Kind KindOf(BlockType t) {
    switch (t) {
    case BlockType::LEVER:
    case BlockType::LEVER_ON: return Kind::Lever;
    case BlockType::PRESSURE_PLATE: return Kind::Plate;
    case BlockType::LOGIC_GATE: return Kind::Gate;
    case BlockType::SIGNAL_LAMP:
    case BlockType::SIGNAL_LAMP_ON: return Kind::Lamp;
    case BlockType::VALVE:
    case BlockType::VALVE_OPEN: return Kind::Valve;
    case BlockType::SLUICE: return Kind::Sluice;
    case BlockType::PISTON: return Kind::Piston;
    case BlockType::WATER_SENSOR: return Kind::WaterSensor;
    case BlockType::PRESSURE_SENSOR: return Kind::PressureSensor;
    case BlockType::DAYLIGHT_SENSOR: return Kind::DaylightSensor;
    case BlockType::PLAYER_SENSOR: return Kind::PlayerSensor;
    default: return Kind::None;
    }
}

// What a block's channels are: `out` for senders, `a` for listeners, and a gate's
// mode with its inputs a and b and its output. A pressure or player sensor keeps its
// setting (percent, or blocks) in b.
struct Config {
    Kind kind = Kind::None;
    Mode mode = Mode::And;
    uint8_t a = 0, b = 0, out = 0;
};

inline bool IsSensor(Kind k) { return k == Kind::WaterSensor || k == Kind::PressureSensor || k == Kind::DaylightSensor || k == Kind::PlayerSensor; }
inline bool Sends(Kind k) { return k == Kind::Lever || k == Kind::Plate || k == Kind::Gate || IsSensor(k); }
inline bool Listens(Kind k) { return k == Kind::Lamp || k == Kind::Valve || k == Kind::Sluice || k == Kind::Gate || k == Kind::Piston; }
// Whether a kind has a setting in b (besides a gate's second input).
inline bool HasSetting(Kind k) { return k == Kind::PressureSensor || k == Kind::PlayerSensor; }
// A player sensor's range for its setting.
inline int PlayerRange(uint8_t b) { return b == 0 ? kPlayerRange : b > kMaxPlayerRange ? kMaxPlayerRange : b; }
// How many inputs a gate in this mode uses.
inline int Inputs(Mode m) { return m == Mode::And || m == Mode::Or || m == Mode::Xor || m == Mode::Latch ? 2 : 1; }

inline const char* ModeName(Mode m) {
    switch (m) {
    case Mode::And: return "AND";
    case Mode::Or: return "OR";
    case Mode::Xor: return "XOR";
    case Mode::Not: return "NOT";
    case Mode::Relay: return "RELAY";
    case Mode::Clock: return "CLOCK";
    case Mode::Latch: return "LATCH";
    default: return "?";
    }
}

// A gate's output for what it hears, given what it's sending now (only a latch
// remembers; a clock is handled by the server).
inline bool Evaluate(Mode m, bool a, bool b, bool on = false) {
    switch (m) {
    case Mode::And: return a && b;
    case Mode::Or: return a || b;
    case Mode::Xor: return a != b;
    case Mode::Not: return !a;
    case Mode::Relay: return a;
    case Mode::Latch: return !b && (a || on);
    default: return false;
    }
}

} // namespace Signals
