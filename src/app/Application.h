#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/Camera.h"
#include "app/Player.h"
#include "app/Settings.h"
#include "world/Block.h"
#include "world/Mining.h"

struct GLFWwindow;
class Audio;
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
    // Start at the title screen (worlds, servers, settings) instead of straight in a
    // game: what running with no command-line options does.
    bool showMenu = false;
    bool nameGiven = false; // --name: otherwise the name from the settings
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
    // A game: connect (starting the local server for singleplayer), and leave it.
    // StartGame returns false (with menuMessage set) if it couldn't.
    bool StartGame(const ClientOptions& options);
    void StopGame();
    void SetCursorCaptured(bool captured);
    void ApplySettings();
    // Menus (app/Menu.cpp): the title screen and its pages, without a game; the pause
    // menu (Esc) over one.
    void RunMenuFrame(double now);
    void DrawMenu();
    void DrawPauseMenu();
    void DrawSettingsPage(bool inGame);
    void RefreshWorlds();
    void PlayWorld(const std::string& directory, bool empty, int mode);
    void LoadBlockTextures();
    void DrawHotbar();
    BlockType SelectedBlock() const;
    Tool SelectedTool() const { return hotbarTools[selectedSlot]; }
    // Survival: holding the left button mines the block in view (world/Mining.h).
    void UpdateMining();
    // Sound effects for this frame: footsteps, others' edits, creatures, rain...
    void UpdateSounds();
    // A sound at a block (its centre); a block's break sound.
    void PlayAt(const char* name, const glm::ivec3& block, float volume = 1.0f);
    static const char* BreakSound(BlockType type);
    // The creature the view ray meets within reach, before any block (0: none).
    // What's in the crosshair to hit, nearer than any block: a creature or another
    // player (at most one is set; 0 and -1 when neither).
    struct FightTarget {
        uint16_t creature = 0;
        int player = -1;
        bool Any() const { return creature != 0 || player >= 0; }
    };
    FightTarget TargetInView() const;
    // How far you can break and build (by game mode).
    float Reach() const;
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
    // Other players' names over their heads (`viewProjection`: this frame's camera).
    void DrawNameTags(const glm::mat4& viewProjection);
    // A small cross in the middle of the screen while aiming.
    void DrawCrosshair();
    void OpenChat(char openedWith);
    void CloseChat();

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
    std::unique_ptr<Audio> audio;
    bool joined = false; // Welcome received and handled
    // Sounds: where our feet were and how far they've walked since the last step,
    // others' steps likewise, creature states (hurt, next groan).
    glm::vec3 lastFeet{ 0.0f };
    float stepDistance = 0.0f;
    bool wasSwimming = false;
    double lastHurtHeard = 0.0, nextDig = 0.0, nextChip = 0.0;
    double nextAmbience = 0.0; // birdsong by day, crickets at night
    // Bits of broken blocks, flying and falling (client-side only: just for show).
    struct Particle {
        glm::vec3 position, velocity;
        float life, maxLife, size;
        glm::vec4 uv;
        bool rise = false; // smoke: drifts up instead of falling
    };
    std::vector<Particle> particles;
    std::vector<glm::ivec3> campfires; // near the player, for smoke
    double nextFireScan = 0.0;
    double nextShot = 0.0; // the bow's next shot
    std::optional<glm::ivec3> lastBobber; // where our bobber was (for the splash when a fish bites)
    std::map<std::pair<int, int>, glm::vec3> arrowFacing; // which way arrows were going before they stuck
    // Lightning: the bolts still showing, how bright the flash is (0..1), and thunder
    // on its way (when it's heard, how loud).
    struct Bolt { std::vector<glm::vec3> points; float life; };
    std::vector<Bolt> bolts;
    float lightningFlash = 0.0f;
    std::vector<std::pair<double, float>> thunder;
    void UpdateLightning(float dt);
    float shake = 0.0f; // the camera shaking from a blast nearby (0..1, fading)
    // Fireflies over grass on clear nights near the player (just for show).
    struct Firefly { glm::vec3 home, position; float phase, life; };
    std::vector<Firefly> fireflies;
    void UpdateFireflies(float dt);
    // Throws `count` bits of this block out of the cell at `pos` (a burst when it breaks,
    // a few while it's mined).
    void SpawnParticles(const glm::ivec3& pos, BlockType type, int count, float speed = 3.0f);
    void UpdateParticles(float dt);
    struct Walker { glm::vec3 feet{ 0.0f }; float distance = 0.0f; bool seen = false; bool hurt = false; };
    std::unordered_map<uint16_t, Walker> walkers;
    struct CreatureSound { bool hurt = false; double nextGroan = 0.0; bool seen = false; };
    std::unordered_map<uint16_t, CreatureSound> creatureSounds;
    bool mute = false; // VOXEL_MUTE
    std::string serverLabel;

    // Menus. Without a game (client), the title screen shows one of these pages.
    enum class Page { Title, Worlds, NewWorld, Multiplayer, Settings };
    Page page = Page::Title;
    bool paused = false;          // in a game: the pause menu is open
    bool pendingStop = false;     // leave the game at the end of this frame
    bool pauseSettings = false;   // ...showing the settings
    Settings settings;
    std::string menuMessage;      // e.g. why a game couldn't start
    struct WorldInfo {
        std::string name, directory;
        bool empty = false;
        int mode = 0;             // the world's default net::GameMode
        long long modified = 0;   // world.dat's last write, for sorting (newest first)
    };
    std::vector<WorldInfo> worlds;
    std::string deleteCandidate;  // a world waiting for "really delete?"
    char newWorldName[64] = "New World";
    int newWorldType = 0, newWorldMode = 0;
    char serverAddress[128] = "";

    bool captureCursor = true;
    bool wireframe = false;
    bool ambientOcclusion = true;
    // Chat: open while typing a line (T, / or Enter opens it).
    bool chatOpen = false;
    bool cursorBeforeChat = true; // captured before the chat opened (given back on closing)
    char chatInput[260] = {};
    char chatOpenedWith = 0; // the key that opened it ('t', '/' or 0), see DrawChat
    int chatOpenFrames = 0;
    bool sheltered = false; // something overhead: no rain falls here
    float rainHere = 0.0f;  // rain drawn around the camera, eased toward the weather
    static constexpr int kHotbarSlots = 10; // keys 1-9 and 0
    std::array<BlockType, kHotbarSlots> hotbar = { BlockType::DIRT, BlockType::STONE, BlockType::GRASS, BlockType::LAMP, BlockType::LOG,
                                                   BlockType::LEAVES, BlockType::SAND, BlockType::WATER, BlockType::STONE_BRICK, BlockType::PUMP };
    // A slot holding a tool (not Tool::None) holds that instead of its block.
    std::array<Tool, kHotbarSlots> hotbarTools{};
    int selectedSlot = 0; // index into the hotbar
    // Mining (survival): the block being broken, with what, since when (glfwGetTime).
    bool mining = false;
    glm::ivec3 miningPos{ 0 };
    Block miningBlock;
    Tool miningTool = Tool::None;
    double miningStart = 0.0, nextMineAt = 0.0;
    float mineProgress = 0.0f; // 0..1
    int placeHeight = 8;  // eighths: how tall shapeable blocks are placed (V / Shift+V)
    bool inventoryOpen = false;
    bool cursorBeforeInventory = true;
    // A chest is open (in place of the inventory): asked for at chestAskedAt, and seen
    // open by the server since.
    bool chestMode = false, chestSeenOpen = false;
    // Writing on a sign (in place of the inventory): which one, and the text so far.
    bool signMode = false;
    glm::ivec3 signAt{ 0 };
    char signText[72] = {};
    void OpenSign(const glm::ivec3& pos);
    void DrawSignEditor();
    void DrawSigns(const glm::mat4& viewProjection); // the texts, floating over their signs
    double chestAskedAt = 0.0;
    void DrawChest();
    double scrollAccumulator = 0.0;
    bool showDebug = false;
    bool showCompass = true; // F4: the compass, coordinates and clock at the top
    // Walking bobs the view (how far into the stride), and sprinting widens it a little.
    float bobPhase = 0.0f, bobAmount = 0.0f, fovBoost = 0.0f;
    void DrawCompass();
    // The minimap (M): a top-down picture of the ground around the player, refreshed
    // a few rows a frame into a small texture.
    static constexpr int kMapSize = 96;
    bool showMap = true;
    unsigned int mapTexture = 0;
    std::vector<uint32_t> mapPixels;
    int mapRow = 0;
    glm::ivec2 mapCentre{ 0 };   // the column the picture is centred on
    void DrawMinimap();
    // The newest achievement toast (from chat), shown for a few seconds.
    std::string toastText;
    double toastUntil = 0.0;
    size_t chatSeen = 0;
    void DrawToast();
    bool vsync = true;
    // VOXEL_BENCH: per-section frame timing, printed once a second.
    bool bench = false;
    bool ignoreMouse = false; // VOXEL_NO_MOUSE
    bool holdMine = false;    // VOXEL_HOLD_MINE: mine as if the left button were held
    std::string startCommands; // VOXEL_RUN: chat lines sent once joined, ';' between them
    double landedAt = 0.0;     // when we first stood on the ground (glfwGetTime)
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
