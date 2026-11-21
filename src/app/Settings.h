#pragma once
// The player's settings, kept in settings.txt in the working directory as
// "key value" lines (unknown keys are ignored, missing ones keep their defaults).
#include <algorithm>
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
    std::string lastServer = "localhost";

    static constexpr int kMinRenderDistance = 2, kMaxRenderDistance = 12;
    static constexpr float kMinFov = 30.0f, kMaxFov = 100.0f;
    static constexpr float kMinSensitivity = 0.02f, kMaxSensitivity = 0.5f;

    void Clamp() {
        renderDistance = std::clamp(renderDistance, kMinRenderDistance, kMaxRenderDistance);
        fov = std::clamp(fov, kMinFov, kMaxFov);
        sensitivity = std::clamp(sensitivity, kMinSensitivity, kMaxSensitivity);
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
            else if (key == "last_server") lastServer = rest;
        }
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
             << "last_server " << lastServer << "\n";
    }
};
