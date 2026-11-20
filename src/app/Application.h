#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "app/Camera.h"
#include "app/Player.h"
#include "world/Block.h"

struct GLFWwindow;
class Client;
class ClientWorld;
class Renderer;
class Server;

// How the game connects: to an in-process server (singleplayer, the default) or to a
// server elsewhere. Either way the game is a network client of that server.
struct ClientOptions {
    std::string host;  // empty: start a local server and connect to it
    uint16_t port = 0; // 0: the default port
    std::string name = "Player";
    // Singleplayer: where the local server saves the world (empty: don't save).
    std::string worldDirectory = "saves/singleplayer";
    int waterSimulationDistance = 6; // singleplayer's local server
    bool emptyWorld = false;         // singleplayer: a new world is a flat, empty plain
};

class Application
{
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool Init(const ClientOptions& options);
    void Run();

private:
    void Shutdown();
    void LoadBlockTextures();
    void DrawHotbar();
    BlockType SelectedBlock() const;
    void SelectSlot(int slot);

    void HandleContinuousInput(float deltaTime);
    void UpdateNetwork();
    void DrawDebugWindow(float deltaTime);
    void DrawStatus();

    void OnKey(int key, int action);
    void OnMouseButton(int button, int action);
    void OnCursorMove(double x, double y);
    void OnScroll(double yOffset);

    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* window, double x, double y);
    static void ScrollCallback(GLFWwindow* window, double xOffset, double yOffset);

    GLFWwindow* window = nullptr;
    bool imguiInitialized = false;

    Camera camera;
    Player player{ glm::vec3(7.0f, 7.0f, 7.0f) };
    // Singleplayer only: the server this game connects to, in this process.
    std::unique_ptr<Server> localServer;
    // Both own GL resources, so they're destroyed explicitly before the context.
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<ClientWorld> world;
    std::unique_ptr<Client> client;
    bool joined = false; // Welcome received and handled
    std::string serverLabel;

    bool captureCursor = true;
    bool wireframe = false;
    bool ambientOcclusion = true;
    float sunBrightness = 1.0f;
    int selectedSlot = 0; // index into the hotbar (kPlaceableBlocks)
    double scrollAccumulator = 0.0;
    bool showDebug = false;
    bool vsync = true;
    // VOXEL_BENCH: per-section frame timing, printed once a second.
    bool bench = false;
    bool ignoreMouse = false; // VOXEL_NO_MOUSE
    double benchSections[7] = {};
    double benchLast = 0.0;
    int benchFrames = 0;
    bool screenshotRequested = false;
    double autoScreenshotAt = 0.0;
    std::string autoScreenshotPath;

    static constexpr int kFrameHistory = 100;
    std::array<float, kFrameHistory> frameTimes{};
    int frameTimeIndex = 0;
    float frameSampleTimer = 0.0f;
};
