// The title screen (singleplayer worlds, multiplayer, settings) and the pause menu.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <GLFW/glfw3.h>
#include "ui/Ui.h"

#include "app/Application.h"
#include "client/Client.h"
#include "net/Protocol.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kSavesDirectory = "saves";
constexpr float kButtonWidth = 300.0f;

// A window in the middle of the screen, sized to fit.
bool BeginCentred(const char* title, float width, int extra = 0) {
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ui::Cond_Always, ui::Vec2(0.5f, 0.5f));
    ui::SetNextWindowSizeConstraints(ui::Vec2(width, 0.0f), ui::Vec2(width, viewport->WorkSize.y * 0.9f));
    ui::SetNextWindowBgAlpha(0.88f);
    const int flags = ui::WindowFlags_NoCollapse | ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove |
                                   ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoResize | extra;
    return ui::Begin(title, nullptr, flags);
}

bool WideButton(const char* label) {
    ui::SetCursorPosX((ui::GetWindowWidth() - kButtonWidth) * 0.5f);
    return ui::Button(label, ui::Vec2(kButtonWidth, 0.0f));
}

void CentredText(const char* text, float scale = 1.0f) {
    ui::SetWindowFontScale(scale);
    ui::SetCursorPosX((ui::GetWindowWidth() - ui::CalcTextSize(text).x) * 0.5f);
    ui::TextUnformatted(text);
    ui::SetWindowFontScale(1.0f);
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
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::GetBackgroundDrawList()->AddRectFilledMultiColor(
        viewport->WorkPos, ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
        UI_COL32(70, 120, 200, 255), UI_COL32(70, 120, 200, 255), UI_COL32(170, 205, 235, 255), UI_COL32(170, 205, 235, 255));

    switch (page) {
    case Page::Title: {
        if (BeginCentred("##title", 380.0f, ui::WindowFlags_NoTitleBar)) {
            ui::Spacing();
            CentredText("VoxelEngine", 2.6f);
            ui::Spacing();
            ui::Spacing();
            if (WideButton("Singleplayer")) {
                RefreshWorlds();
                page = Page::Worlds;
            }
            if (WideButton("Multiplayer")) page = Page::Multiplayer;
            if (WideButton("Settings")) page = Page::Settings;
            if (WideButton("Quit")) glfwSetWindowShouldClose(window, GLFW_TRUE);
            ui::Spacing();
            ui::TextDisabled("Playing as %s", settings.name.c_str());
            if (!menuMessage.empty()) ui::TextColored(ui::Vec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", menuMessage.c_str());
        }
        ui::End();
        break;
    }
    case Page::Worlds: {
        if (BeginCentred("Singleplayer", 520.0f)) {
            if (worlds.empty()) ui::TextDisabled("No worlds yet. Make one below.");
            if (ui::BeginTable("worlds", 3, ui::TableFlags_RowBg | ui::TableFlags_BordersInnerH)) {
                ui::TableSetupColumn("World", ui::TableColumnFlags_WidthStretch);
                ui::TableSetupColumn("Kind", ui::TableColumnFlags_WidthFixed, 130.0f);
                ui::TableSetupColumn("", ui::TableColumnFlags_WidthFixed, 120.0f);
                for (const auto& w : worlds) {
                    ui::PushID(w.directory.c_str());
                    ui::TableNextRow();
                    ui::TableNextColumn();
                    ui::AlignTextToFramePadding();
                    ui::TextUnformatted(w.name.c_str());
                    ui::TableNextColumn();
                    ui::TextDisabled("%s, %s", w.empty ? "empty" : "normal", w.mode == 1 ? "creative" : "survival");
                    ui::TableNextColumn();
                    if (ui::SmallButton("Play")) PlayWorld(w.directory, w.empty, -1);
                    ui::SameLine();
                    if (ui::SmallButton("Delete")) {
                        deleteCandidate = w.directory;
                        ui::OpenPopup("Delete world?");
                    }
                    if (ui::BeginPopupModal("Delete world?", nullptr, ui::WindowFlags_AlwaysAutoResize)) {
                        ui::Text("Delete \"%s\" for good? This can't be undone.", w.name.c_str());
                        if (ui::Button("Delete it")) {
                            std::error_code ec;
                            fs::remove_all(deleteCandidate, ec);
                            if (ec) menuMessage = "Couldn't delete " + deleteCandidate + ": " + ec.message();
                            deleteCandidate.clear();
                            RefreshWorlds();
                            ui::CloseCurrentPopup();
                        }
                        ui::SameLine();
                        if (ui::Button("Keep it")) {
                            deleteCandidate.clear();
                            ui::CloseCurrentPopup();
                        }
                        ui::EndPopup();
                    }
                    ui::PopID();
                    if (client) break; // a world started: the list is gone
                }
                ui::EndTable();
            }
            ui::Spacing();
            if (!menuMessage.empty()) ui::TextColored(ui::Vec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", menuMessage.c_str());
            if (WideButton("New world")) page = Page::NewWorld;
            if (WideButton("Back")) page = Page::Title;
        }
        ui::End();
        break;
    }
    case Page::NewWorld: {
        if (BeginCentred("New world", 420.0f)) {
            ui::InputText("Name", newWorldName, sizeof newWorldName);
            const char* types[] = { "Normal", "Empty" };
            ui::Combo("Terrain", &newWorldType, types, 2);
            ui::TextDisabled(newWorldType == 0 ? "Hills, biomes, caves, dungeons and creatures." : "A flat plain, for building and testing.");
            const char* modes[] = { "Survival", "Creative" };
            ui::Combo("Mode", &newWorldMode, modes, 2);
            ui::TextDisabled(newWorldMode == 0 ? "Collect what you build with; mind your health." : "Build with anything; nothing hurts you.");
            ui::Spacing();
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
        ui::End();
        break;
    }
    case Page::Multiplayer: {
        if (BeginCentred("Multiplayer", 420.0f)) {
            ui::TextWrapped("Join a server (VoxelServer, or VoxelEngine --server). The default port is %u.", net::kDefaultPort);
            ui::Spacing();
            ui::InputText("Address", serverAddress, sizeof serverAddress);
            ui::TextDisabled("host or host:port");
            if (!menuMessage.empty()) ui::TextColored(ui::Vec4(1.0f, 0.5f, 0.45f, 1.0f), "%s", menuMessage.c_str());
            ui::Spacing();
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
        ui::End();
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
        if (std::strcmp(name, settings.name.c_str()) != 0 && !ui::IsAnyItemActive())
            std::snprintf(name, sizeof name, "%s", settings.name.c_str());
        if (ui::InputText("Name", name, sizeof name)) settings.name = name;
        if (inGame) ui::TextDisabled("(a new name counts from the next game)");
        ui::SliderInt("Render distance", &settings.renderDistance, Settings::kMinRenderDistance, Settings::kMaxRenderDistance, "%d chunks");
        ui::TextDisabled("Further shows more, nearer runs faster.%s", inGame ? " From the next game." : "");
        if (ui::SliderFloat("Field of view", &settings.fov, Settings::kMinFov, Settings::kMaxFov, "%.0f degrees")) ApplySettings();
        if (ui::SliderFloat("Mouse sensitivity", &settings.sensitivity, Settings::kMinSensitivity, Settings::kMaxSensitivity, "%.2f"))
            ApplySettings();
        if (ui::Checkbox("Vsync", &settings.vsync)) ApplySettings();
        ui::Checkbox("View bobbing", &settings.viewBobbing);
        int volume = static_cast<int>(settings.volume * 100.0f + 0.5f);
        if (ui::SliderInt("Volume", &volume, 0, 100, "%d%%")) settings.volume = volume / 100.0f;
        // (VOXEL_MENU_PAGE=controls opens the settings with this open, for screenshots.)
        static const bool openControls = [] {
            const char* p = std::getenv("VOXEL_MENU_PAGE");
            return p && std::string(p) == "controls";
        }();
        if (openControls) ui::SetNextItemOpen(true, ui::Cond_Once);
        if (ui::CollapsingHeader("Controls")) {
            ui::TextDisabled("Click one, then press its new key (Esc keeps the old one).");
            if (ui::BeginTable("keys", 2, ui::TableFlags_SizingStretchProp)) {
                for (int i = 0; i < kActionCount; i++) {
                    ui::TableNextRow();
                    ui::TableNextColumn();
                    ui::AlignTextToFramePadding();
                    ui::TextUnformatted(Bindings::Name(static_cast<Action>(i)));
                    ui::TableNextColumn();
                    ui::PushID(i);
                    // (A key bound twice is shown in red.)
                    bool twice = false;
                    for (int j = 0; j < kActionCount; j++) twice |= j != i && settings.keys[static_cast<size_t>(j)] == settings.keys[static_cast<size_t>(i)];
                    if (twice) ui::PushStyleColor(ui::Col_Text, ui::Vec4(1.0f, 0.45f, 0.4f, 1.0f));
                    const std::string label = rebinding == i ? "press a key..." : Bindings::KeyName(settings.keys[static_cast<size_t>(i)]);
                    if (ui::Button(label.c_str(), ui::Vec2(140.0f, 0.0f))) rebinding = i;
                    if (twice) ui::PopStyleColor();
                    ui::PopID();
                }
                ui::EndTable();
            }
            if (ui::Button("Reset to the defaults")) settings.keys = Bindings::Defaults();
            ui::TextDisabled("Fixed: 1-0 hotbar, Tab frees the mouse, Esc pauses, F1-F4 and F12, mouse buttons.");
            ui::Spacing();
            if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1)) ui::Text("Gamepad: %s", glfwGetGamepadName(GLFW_JOYSTICK_1));
            else ui::TextDisabled("No gamepad connected.");
            ui::SliderFloat("Gamepad look speed", &settings.padLook, Settings::kMinPadLook, Settings::kMaxPadLook, "%.0f deg/s");
            ui::TextDisabled("Left stick walks, right stick looks; A jumps, B crouches, left stick (pressed) sprints;\n"
                                "right trigger mines and hits, left trigger places and uses; bumpers change slot;\n"
                                "Y inventory, Back map, Start pause.");
        }
        ui::Spacing();
        if (WideButton("Done")) {
            settings.Clamp();
            settings.Save(kSettingsFile);
            if (inGame) pauseSettings = false;
            else page = Page::Title;
        }
    }
    ui::End();
}

void Application::DrawPauseMenu() {
    // The game dimmed behind.
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::GetBackgroundDrawList()->AddRectFilled(viewport->WorkPos,
                                                  ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
                                                  UI_COL32(0, 0, 0, 110));
    if (pauseSettings) {
        DrawSettingsPage(true);
        return;
    }
    const bool connected = client->IsConnected();
    if (BeginCentred(connected ? "Paused" : "Disconnected", 360.0f)) {
        if (!connected) ui::TextWrapped("%s", client->GetDisconnectReason().c_str());
        else ui::TextDisabled(localServer ? "Singleplayer (the world keeps going)" : "%s", serverLabel.c_str());
        ui::Spacing();
        if (connected && WideButton("Back to the game")) {
            paused = false;
            SetCursorCaptured(true);
        }
        if (connected && WideButton("Settings")) pauseSettings = true;
        if (WideButton(localServer ? "Save and quit to title" : connected ? "Disconnect" : "Back to title")) pendingStop = true;
        if (WideButton("Quit the game")) glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
    ui::End();
}
