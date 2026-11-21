// The title screen (singleplayer worlds, multiplayer, settings) and the pause menu.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include "app/Application.h"
#include "client/Client.h"
#include "net/Protocol.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kSavesDirectory = "saves";
constexpr float kButtonWidth = 300.0f;

// A window in the middle of the screen, sized to fit.
bool BeginCentred(const char* title, float width, ImGuiWindowFlags extra = 0) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, viewport->WorkSize.y * 0.9f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize | extra;
    return ImGui::Begin(title, nullptr, flags);
}

bool WideButton(const char* label) {
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - kButtonWidth) * 0.5f);
    return ImGui::Button(label, ImVec2(kButtonWidth, 0.0f));
}

void CentredText(const char* text, float scale = 1.0f) {
    ImGui::SetWindowFontScale(scale);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(text).x) * 0.5f);
    ImGui::TextUnformatted(text);
    ImGui::SetWindowFontScale(1.0f);
}

// A world's header from its world.dat (see WorldStorage): format, type, default mode.
bool ReadWorldHeader(const fs::path& file, bool& empty, int& mode) {
    std::ifstream in(file, std::ios::binary);
    char magic[4];
    uint8_t version[4];
    if (!in.read(magic, 4) || std::memcmp(magic, "VXWD", 4) != 0 || !in.read(reinterpret_cast<char*>(version), 4)) return false;
    const uint32_t v = version[0] | (version[1] << 8) | (version[2] << 16) | (static_cast<uint32_t>(version[3]) << 24);
    uint8_t type = 0, defaultMode = 1;
    float time;
    if (v >= 2) in.read(reinterpret_cast<char*>(&type), 1);
    if (v >= 3) in.read(reinterpret_cast<char*>(&time), 4);
    if (v >= 4) in.read(reinterpret_cast<char*>(&defaultMode), 1);
    empty = type == 1;
    mode = defaultMode;
    return static_cast<bool>(in);
}

// A folder name for a world: letters, digits, spaces, '-' and '_'.
std::string FolderName(const std::string& name) {
    std::string out;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? "World" : out;
}

} // namespace

void Application::RefreshWorlds() {
    worlds.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(kSavesDirectory, ec)) {
        if (!entry.is_directory(ec)) continue;
        const fs::path dat = entry.path() / "world.dat";
        WorldInfo info;
        if (!fs::exists(dat, ec) || !ReadWorldHeader(dat, info.empty, info.mode)) continue;
        info.name = entry.path().filename().string();
        info.directory = entry.path().string();
        info.modified = fs::last_write_time(dat, ec).time_since_epoch().count();
        worlds.push_back(info);
    }
    std::sort(worlds.begin(), worlds.end(), [](const WorldInfo& a, const WorldInfo& b) { return a.modified > b.modified; });
}

void Application::PlayWorld(const std::string& directory, bool empty, int mode) {
    ClientOptions options;
    options.name = settings.name;
    options.worldDirectory = directory;
    options.emptyWorld = empty;
    options.mode = mode;
    if (!StartGame(options)) page = Page::Worlds;
}

void Application::DrawMenu() {
    // A sky behind it all.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(
        viewport->WorkPos, ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
        IM_COL32(70, 120, 200, 255), IM_COL32(70, 120, 200, 255), IM_COL32(170, 205, 235, 255), IM_COL32(170, 205, 235, 255));

    switch (page) {
    case Page::Title: {
        if (BeginCentred("##title", 380.0f, ImGuiWindowFlags_NoTitleBar)) {
            ImGui::Spacing();
            CentredText("VoxelEngine", 2.6f);
            ImGui::Spacing();
            ImGui::Spacing();
            if (WideButton("Singleplayer")) {
                RefreshWorlds();
                page = Page::Worlds;
            }
            if (WideButton("Multiplayer")) page = Page::Multiplayer;
            if (WideButton("Settings")) page = Page::Settings;
            if (WideButton("Quit")) glfwSetWindowShouldClose(window, GLFW_TRUE);
            ImGui::Spacing();
            ImGui::TextDisabled("Playing as %s", settings.name.c_str());
            if (!menuMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", menuMessage.c_str());
        }
        ImGui::End();
        break;
    }
    case Page::Worlds: {
        if (BeginCentred("Singleplayer", 520.0f)) {
            if (worlds.empty()) ImGui::TextDisabled("No worlds yet. Make one below.");
            if (ImGui::BeginTable("worlds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("World", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                for (const auto& w : worlds) {
                    ImGui::PushID(w.directory.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(w.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s, %s", w.empty ? "empty" : "normal", w.mode == 1 ? "creative" : "survival");
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Play")) PlayWorld(w.directory, w.empty, -1);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) {
                        deleteCandidate = w.directory;
                        ImGui::OpenPopup("Delete world?");
                    }
                    if (ImGui::BeginPopupModal("Delete world?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                        ImGui::Text("Delete \"%s\" for good? This can't be undone.", w.name.c_str());
                        if (ImGui::Button("Delete it")) {
                            std::error_code ec;
                            fs::remove_all(deleteCandidate, ec);
                            if (ec) menuMessage = "Couldn't delete " + deleteCandidate + ": " + ec.message();
                            deleteCandidate.clear();
                            RefreshWorlds();
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Keep it")) {
                            deleteCandidate.clear();
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                    if (client) break; // a world started: the list is gone
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            if (!menuMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", menuMessage.c_str());
            if (WideButton("New world")) page = Page::NewWorld;
            if (WideButton("Back")) page = Page::Title;
        }
        ImGui::End();
        break;
    }
    case Page::NewWorld: {
        if (BeginCentred("New world", 420.0f)) {
            ImGui::InputText("Name", newWorldName, sizeof newWorldName);
            const char* types[] = { "Normal", "Empty" };
            ImGui::Combo("Terrain", &newWorldType, types, 2);
            ImGui::TextDisabled(newWorldType == 0 ? "Hills, biomes, caves, dungeons and creatures." : "A flat plain, for building and testing.");
            const char* modes[] = { "Survival", "Creative" };
            ImGui::Combo("Mode", &newWorldMode, modes, 2);
            ImGui::TextDisabled(newWorldMode == 0 ? "Collect what you build with; mind your health." : "Build with anything; nothing hurts you.");
            ImGui::Spacing();
            if (WideButton("Create and play")) {
                // A folder of its own (a number added if the name's taken).
                std::string folder = FolderName(newWorldName);
                fs::path path = fs::path(kSavesDirectory) / folder;
                std::error_code ec;
                for (int n = 2; fs::exists(path, ec); n++) path = fs::path(kSavesDirectory) / (folder + " " + std::to_string(n));
                PlayWorld(path.string(), newWorldType == 1, newWorldMode == 1 ? static_cast<int>(net::GameMode::Creative)
                                                                              : static_cast<int>(net::GameMode::Survival));
            }
            if (WideButton("Back")) page = Page::Worlds;
        }
        ImGui::End();
        break;
    }
    case Page::Multiplayer: {
        if (BeginCentred("Multiplayer", 420.0f)) {
            ImGui::TextWrapped("Join a server (VoxelServer, or VoxelEngine --server). The default port is %u.", net::kDefaultPort);
            ImGui::Spacing();
            ImGui::InputText("Address", serverAddress, sizeof serverAddress);
            ImGui::TextDisabled("host or host:port");
            if (!menuMessage.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", menuMessage.c_str());
            ImGui::Spacing();
            if (WideButton("Join")) {
                std::string host = serverAddress;
                uint16_t port = net::kDefaultPort;
                const size_t colon = host.rfind(':');
                if (colon != std::string::npos) {
                    port = static_cast<uint16_t>(std::atoi(host.c_str() + colon + 1));
                    host = host.substr(0, colon);
                }
                if (host.empty() || port == 0) menuMessage = "That isn't an address";
                else {
                    settings.lastServer = serverAddress;
                    settings.Save(kSettingsFile);
                    ClientOptions options;
                    options.host = host;
                    options.port = port;
                    options.name = settings.name;
                    StartGame(options);
                }
            }
            if (WideButton("Back")) page = Page::Title;
        }
        ImGui::End();
        break;
    }
    case Page::Settings:
        DrawSettingsPage(false);
        break;
    }
}

void Application::DrawSettingsPage(bool inGame) {
    if (BeginCentred("Settings", 440.0f)) {
        static char name[32];
        if (std::strcmp(name, settings.name.c_str()) != 0 && !ImGui::IsAnyItemActive())
            std::snprintf(name, sizeof name, "%s", settings.name.c_str());
        if (ImGui::InputText("Name", name, sizeof name)) settings.name = name;
        if (inGame) ImGui::TextDisabled("(a new name counts from the next game)");
        ImGui::SliderInt("Render distance", &settings.renderDistance, Settings::kMinRenderDistance, Settings::kMaxRenderDistance, "%d chunks");
        ImGui::TextDisabled("Further shows more, nearer runs faster.%s", inGame ? " From the next game." : "");
        if (ImGui::SliderFloat("Field of view", &settings.fov, Settings::kMinFov, Settings::kMaxFov, "%.0f degrees")) ApplySettings();
        if (ImGui::SliderFloat("Mouse sensitivity", &settings.sensitivity, Settings::kMinSensitivity, Settings::kMaxSensitivity, "%.2f"))
            ApplySettings();
        if (ImGui::Checkbox("Vsync", &settings.vsync)) ApplySettings();
        ImGui::Spacing();
        if (WideButton("Done")) {
            settings.Clamp();
            settings.Save(kSettingsFile);
            if (inGame) pauseSettings = false;
            else page = Page::Title;
        }
    }
    ImGui::End();
}

void Application::DrawPauseMenu() {
    // The game dimmed behind.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->WorkPos,
                                                  ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
                                                  IM_COL32(0, 0, 0, 110));
    if (pauseSettings) {
        DrawSettingsPage(true);
        return;
    }
    const bool connected = client->IsConnected();
    if (BeginCentred(connected ? "Paused" : "Disconnected", 360.0f)) {
        if (!connected) ImGui::TextWrapped("%s", client->GetDisconnectReason().c_str());
        else ImGui::TextDisabled(localServer ? "Singleplayer (the world keeps going)" : "%s", serverLabel.c_str());
        ImGui::Spacing();
        if (connected && WideButton("Back to the game")) {
            paused = false;
            SetCursorCaptured(true);
        }
        if (connected && WideButton("Settings")) pauseSettings = true;
        if (WideButton(localServer ? "Save and quit to title" : connected ? "Disconnect" : "Back to title")) pendingStop = true;
        if (WideButton("Quit the game")) glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    ImGui::End();
}
