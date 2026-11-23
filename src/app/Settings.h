#pragma once
// The player's settings, kept in settings.txt in the working directory as
// "key value" lines (unknown keys are ignored, missing ones keep their defaults).
#include <algorithm>
#include <array>

#include "app/Bindings.h"
#include <fstream>
#include <sstream>
#include <string>

inline constexpr const char* kSettingsFile = "settings.txt";

struct Settings {
    std::string name = "Player";
    int renderDistance = 12;  // chunks (the server may send fewer)
    float fov = 45.0f;        // vertical field of view, degrees
    float sensitivity = 0.1f; // mouse: degrees per pixel
    bool vsync = true;
    float volume = 0.8f;      // sound, 0..1
    bool viewBobbing = true;  // the view bobs a little while walking
    std::string lastServer = "localhost";
    std::array<int, kActionCount> keys = Bindings::Defaults(); // (GLFW key codes)
    float padLook = 160.0f;   // a gamepad's right stick: degrees a second, full over
    int bindingsVersion = 2;  // (as read: older files get the newer defaults, see Load)

    static constexpr int kMinRenderDistance = 2, kMaxRenderDistance = 12;
    static constexpr float kMinFov = 30.0f, kMaxFov = 100.0f;
    static constexpr float kMinSensitivity = 0.02f, kMaxSensitivity = 0.5f;
    static constexpr float kMinPadLook = 40.0f, kMaxPadLook = 400.0f;
    int Key(Action a) const { return keys[static_cast<size_t>(a)]; }

    void Clamp() {
        renderDistance = std::clamp(renderDistance, kMinRenderDistance, kMaxRenderDistance);
        fov = std::clamp(fov, kMinFov, kMaxFov);
        sensitivity = std::clamp(sensitivity, kMinSensitivity, kMaxSensitivity);
        volume = std::clamp(volume, 0.0f, 1.0f);
        padLook = std::clamp(padLook, kMinPadLook, kMaxPadLook);
        if (name.empty()) name = "Player";
    }

    bool Load(const std::string& path) {
        std::ifstream file(path);
        if (!file) return false;
        for (std::string line; std::getline(file, line);) {
            std::istringstream in(line);
            std::string key;
            if (!(in >> key)) continue;
            std::string rest;
            std::getline(in >> std::ws, rest);
            if (key == "name") name = rest;
            else if (key == "render_distance") renderDistance = std::atoi(rest.c_str());
            else if (key == "fov") fov = static_cast<float>(std::atof(rest.c_str()));
            else if (key == "sensitivity") sensitivity = static_cast<float>(std::atof(rest.c_str()));
            else if (key == "vsync") vsync = rest != "0";
            else if (key == "volume") volume = static_cast<float>(std::atof(rest.c_str()));
            else if (key == "last_server") lastServer = rest;
            else if (key == "view_bobbing") viewBobbing = rest != "0";
            else if (key == "pad_look") padLook = static_cast<float>(std::atof(rest.c_str()));
            else if (key == "bindings_version") bindingsVersion = std::atoi(rest.c_str());
            else if (key.rfind("key_", 0) == 0)
                for (int i = 0; i < kActionCount; i++)
                    if (key.substr(4) == Bindings::Id(static_cast<Action>(i))) keys[static_cast<size_t>(i)] = std::atoi(rest.c_str());
        }
        // The inventory moved from I to Tab (bindings version 2): a file from before
        // that still holding I gets Tab.
        if (bindingsVersion < 2 && keys[static_cast<size_t>(Action::Inventory)] == GLFW_KEY_I)
            keys[static_cast<size_t>(Action::Inventory)] = GLFW_KEY_TAB;
        bindingsVersion = 2;
        Clamp();
        return true;
    }

    void Save(const std::string& path) const {
        std::ofstream file(path, std::ios::trunc);
        file << "name " << name << "\n"
             << "render_distance " << renderDistance << "\n"
             << "fov " << fov << "\n"
             << "sensitivity " << sensitivity << "\n"
             << "vsync " << (vsync ? 1 : 0) << "\n"
             << "volume " << volume << "\n"
             << "last_server " << lastServer << "\n"
             << "view_bobbing " << (viewBobbing ? 1 : 0) << "\n"
             << "pad_look " << padLook << "\n"
             << "bindings_version 2\n";
        for (int i = 0; i < kActionCount; i++) file << "key_" << Bindings::Id(static_cast<Action>(i)) << " " << keys[static_cast<size_t>(i)] << "\n";
    }
};
