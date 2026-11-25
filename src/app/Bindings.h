#pragma once
// What each key does (rebindable in the settings, kept in settings.txt as
// "key_<action> <GLFW key code>"), and the names keys are shown by.
#include <array>
#include <cstdio>
#include <string>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE // (glad brings the GL header)
#endif
#include <GLFW/glfw3.h>

enum class Action : uint8_t {
    Forward, Back, Left, Right, Jump, Crouch, Sprint, FlyUp, FlyDown,
    Inventory, Chat, Command, Map, Players, PlaceHeight, SkipTime, Reload, Drop, Count
};
constexpr int kActionCount = static_cast<int>(Action::Count);

namespace Bindings {

inline int Default(Action a) {
    switch (a) {
    case Action::Forward: return GLFW_KEY_W;
    case Action::Back: return GLFW_KEY_S;
    case Action::Left: return GLFW_KEY_A;
    case Action::Right: return GLFW_KEY_D;
    case Action::Jump: return GLFW_KEY_SPACE;
    case Action::Crouch: return GLFW_KEY_LEFT_CONTROL;
    case Action::Sprint: return GLFW_KEY_LEFT_SHIFT;
    case Action::FlyUp: return GLFW_KEY_E;
    case Action::FlyDown: return GLFW_KEY_Q;
    case Action::Inventory: return GLFW_KEY_TAB;
    case Action::Chat: return GLFW_KEY_T;
    case Action::Command: return GLFW_KEY_SLASH;
    case Action::Map: return GLFW_KEY_M;
    case Action::Players: return GLFW_KEY_P;
    case Action::PlaceHeight: return GLFW_KEY_V;
    case Action::SkipTime: return GLFW_KEY_N;
    case Action::Reload: return GLFW_KEY_R;
    case Action::Drop: return GLFW_KEY_G;
    default: return GLFW_KEY_UNKNOWN;
    }
}

inline const char* Id(Action a) {
    static const char* ids[] = { "forward", "back", "left", "right", "jump", "crouch", "sprint", "fly_up", "fly_down",
                                 "inventory", "chat", "command", "map", "players", "place_height", "skip_time", "reload", "drop" };
    static_assert(sizeof(ids) / sizeof(ids[0]) == kActionCount, "an action has no id");
    return ids[static_cast<int>(a)];
}

inline const char* Name(Action a) {
    static const char* names[] = { "Walk forward", "Walk back", "Step left", "Step right", "Jump (swim up)", "Crouch (get out of a boat)", "Sprint",
                                   "Fly up", "Fly down", "Inventory", "Chat", "Command (/)", "Map", "Player names",
                                   "Placement height", "Skip to dusk or dawn", "Reload textures and data",
                                   "Drop the item in hand (with Crouch: the stack)" };
    static_assert(sizeof(names) / sizeof(names[0]) == kActionCount, "an action has no name");
    return names[static_cast<int>(a)];
}

inline std::array<int, kActionCount> Defaults() {
    std::array<int, kActionCount> keys{};
    for (int i = 0; i < kActionCount; i++) keys[i] = Default(static_cast<Action>(i));
    return keys;
}

// What a key is called on screen.
inline std::string KeyName(int key) {
    switch (key) {
    case GLFW_KEY_SPACE: return "Space";
    case GLFW_KEY_LEFT_SHIFT: return "Left Shift";
    case GLFW_KEY_RIGHT_SHIFT: return "Right Shift";
    case GLFW_KEY_LEFT_CONTROL: return "Left Ctrl";
    case GLFW_KEY_RIGHT_CONTROL: return "Right Ctrl";
    case GLFW_KEY_LEFT_ALT: return "Left Alt";
    case GLFW_KEY_RIGHT_ALT: return "Right Alt";
    case GLFW_KEY_TAB: return "Tab";
    case GLFW_KEY_ENTER: return "Enter";
    case GLFW_KEY_BACKSPACE: return "Backspace";
    case GLFW_KEY_UP: return "Up";
    case GLFW_KEY_DOWN: return "Down";
    case GLFW_KEY_LEFT: return "Left";
    case GLFW_KEY_RIGHT: return "Right";
    case GLFW_KEY_CAPS_LOCK: return "Caps Lock";
    default: break;
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F25) return "F" + std::to_string(key - GLFW_KEY_F1 + 1);
    if (const char* n = glfwGetKeyName(key, 0)) {
        std::string s = n;
        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }
    char text[16];
    std::snprintf(text, sizeof text, "Key %d", key);
    return text;
}

} // namespace Bindings
