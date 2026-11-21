#pragma once
// Signals: wireless, on numbered channels (1..255; 0 is none). Anything sending on a
// channel is heard by everything listening to that channel within kRange blocks.
//
// Senders:
//  - a lever, while it's on (right-click flips it);
//  - a pressure plate, for kPlateSeconds after someone steps on it (it still opens
//    the old-style sluice gates around it too);
//  - a logic gate, while its output is on.
// Listeners:
//  - a signal lamp lights while its channel is on;
//  - a valve opens while its channel is on (a valve with no channel is opened and
//    shut by hand);
//  - a sluice gate with a channel opens (out of the way, letting water through) while
//    its channel is on and closes again when it's off, pushing any water in its cell
//    up or aside (it waits if there's nowhere for the water to go). A sluice gate with
//    no channel is the old kind: a pressure plate near it opens it for good.
//  - a logic gate's inputs.
// Logic gates work out their output from what they hear on their inputs (a and b),
// each update, and send it on their output channel from the next: AND, OR, XOR, NOT
// (of a), RELAY (a, sent on again: to carry a signal further than kRange), and CLOCK
// (on and off every kClockSeconds while a is on, or always with no a).
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

enum class Kind : uint8_t { None, Lever, Plate, Gate, Lamp, Valve, Sluice };
enum class Mode : uint8_t { And, Or, Xor, Not, Relay, Clock, Count };

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
    default: return Kind::None;
    }
}

// What a block's channels are: `out` for senders, `a` for listeners, and a gate's
// mode with its inputs a and b and its output.
struct Config {
    Kind kind = Kind::None;
    Mode mode = Mode::And;
    uint8_t a = 0, b = 0, out = 0;
};

inline bool Sends(Kind k) { return k == Kind::Lever || k == Kind::Plate || k == Kind::Gate; }
inline bool Listens(Kind k) { return k == Kind::Lamp || k == Kind::Valve || k == Kind::Sluice || k == Kind::Gate; }
// How many inputs a gate in this mode uses.
inline int Inputs(Mode m) { return m == Mode::And || m == Mode::Or || m == Mode::Xor ? 2 : 1; }

inline const char* ModeName(Mode m) {
    switch (m) {
    case Mode::And: return "AND";
    case Mode::Or: return "OR";
    case Mode::Xor: return "XOR";
    case Mode::Not: return "NOT";
    case Mode::Relay: return "RELAY";
    case Mode::Clock: return "CLOCK";
    default: return "?";
    }
}

// A gate's output for what it hears (a clock is handled by the server).
inline bool Evaluate(Mode m, bool a, bool b) {
    switch (m) {
    case Mode::And: return a && b;
    case Mode::Or: return a || b;
    case Mode::Xor: return a != b;
    case Mode::Not: return !a;
    case Mode::Relay: return a;
    default: return false;
    }
}

} // namespace Signals
