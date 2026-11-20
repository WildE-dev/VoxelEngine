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
struct FrameParams;

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
    float dayLengthSeconds = 1200.0f; // singleplayer's local server
    int weather = 0;                  // singleplayer: Server::Options::Weather (0 auto)
    int mode = -1;                    // singleplayer: a new world's net::GameMode (-1: survival, creative if empty)
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
    // Inventory (I): pick which block goes in each hotbar slot; kept in hotbar.txt.
    void DrawInventory();
    // C / Shift+C: lower / raise the quarter of the block in view.
    void ChiselTarget(bool raise);
    void SetInventoryOpen(bool open);
    void LoadHotbar();
    void SaveHotbar() const;

    void HandleContinuousInput(float deltaTime);
    void UpdateNetwork();
    // Sun direction and brightness, sky colour and rain for this frame, from the
    // server's time of day and weather.
    void UpdateSky(float deltaTime, FrameParams& params);
    void DrawDebugWindow(float deltaTime);
    void DrawStatus();
    // The chat log (bottom left) and, while typing, the input line.
    void DrawChat();
    // Hearts above the hotbar, and a red flash when hurt.
    void DrawHealth();
    // A small cross in the middle of the screen while aiming.
    void DrawCrosshair();
    void OpenChat(char openedWith);

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
    // Chat: open while typing a line (T, / or Enter opens it).
    bool chatOpen = false;
    char chatInput[260] = {};
    char chatOpenedWith = 0; // the key that opened it ('t', '/' or 0), see DrawChat
    int chatOpenFrames = 0;
    bool sheltered = false; // something overhead: no rain falls here
    float rainHere = 0.0f;  // rain drawn around the camera, eased toward the weather
    static constexpr int kHotbarSlots = 10; // keys 1-9 and 0
    std::array<BlockType, kHotbarSlots> hotbar = { BlockType::DIRT, BlockType::STONE, BlockType::GRASS, BlockType::LAMP, BlockType::LOG,
                                                   BlockType::LEAVES, BlockType::SAND, BlockType::WATER, BlockType::STONE_BRICK, BlockType::PUMP };
    int selectedSlot = 0; // index into the hotbar
    int placeHeight = 8;  // eighths: how tall shapeable blocks are placed (V / Shift+V)
    bool inventoryOpen = false;
    bool cursorBeforeInventory = true;
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
