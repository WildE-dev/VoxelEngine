#include "app/Application.h"
#include "app/ItemPalette.h"
#include "app/PlayerNames.h"
#include "world/ModelBoxes.h"
#include "world/Trees.h"
#include "world/Trades.h"
#include "world/Dwarves.h"

#include <algorithm>
#include <cfloat>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "ui/Ui.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "audio/Audio.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "net/Protocol.h"
#include "net/Lan.h"
#include "net/RelayHost.h"
#include "render/Renderer.h"
#include "server/Server.h"
#include "world/Achievements.h"
#include "world/Breathing.h"
#include "world/Hunger.h"
#include "world/Legacy.h"
#include "world/BlockData.h"
#include "world/Bodies.h"
#include "world/Multiblocks.h"
#include "world/Power.h"
#include "world/Signals.h"
#include "world/Chunk.h"
#include "world/Crafting.h"
#include "world/Json.h"
#include "world/Smelting.h"
#include "world/Raycast.h"
#include "world/TerrainGenerator.h"

namespace {

constexpr int kWindowWidth = 1200;
constexpr int kWindowHeight = 700;
constexpr const char* kGlslVersion = "#version 330 core";

// How far you can break and build: 6 blocks in survival, 50 in creative.
constexpr float kCreativeReach = 50.0f, kSurvivalReach = 6.0f;
constexpr float kLaserRange = 100.0f;
constexpr int kLaserRadius = 4;
// Where the chosen hotbar is kept between games (working directory).
constexpr const char* kHotbarFile = "hotbar.txt";
constexpr const char* kChatHistoryFile = "chat_history.txt";

// Atlas tile shown for a block in the hotbar and inventory: a sprite's own picture, a
// block's side.
// Resource units as blocks: "12", "3.5", "0.25".
std::string FormatBlocks(uint32_t units) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", units / static_cast<double>(Block::kUnitsPerBlock));
    std::string s = text;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

const char* HeightName(int eighths) {
    switch (eighths) {
    case 2: return "1/4";
    case 4: return "1/2";
    case 6: return "3/4";
    default: return "full";
    }
}

const char* ChiselStepName(int eighths) {
    switch (eighths) {
    case 1: return "1/8";
    case 4: return "1/2";
    default: return "1/4";
    }
}

// A tool's icon (its atlas tile).
int ToolTile(const TextureAtlas& atlas, Tool tool) {
    const char* name = Mining::ToolTexture(tool);
    return name ? atlas.GetIndex(name) : 0;
}
const char* ToolHint(Tool tool) {
    switch (tool) {
    case Tool::Pickaxe: return "fast on stone, bricks, ice and machines";
    case Tool::Axe: return "fast on wood";
    case Tool::Shovel: return "fast on dirt, grass, sand and snow; right-click: flatten (V: how wide)";
    case Tool::Sword: return "for fighting";
    case Tool::Bow: return "right-click to shoot (needs arrows)";
    case Tool::Rod: return "right-click water to cast";
    case Tool::Chisel: return "right-click: cut a corner (crouch: build up); V: how fine";
    default: return "";
    }
}

int IconTile(BlockType type) {
    const TextureData t = GetTextureData(type);
    return IsPlantType(type) ? t.top : t.front;
}

const char* DefaultBlockName(BlockType type);
// What the game calls a block: Resources/Data/blocks.json's name, or its own.
const char* BlockName(BlockType type) { return BlockData::NameOr(type, DefaultBlockName(type)); }

// An item's icon (atlas tile) and name: a block's, an item type's, a tool's, a piece of
// armour's.
int ItemTile(const TextureAtlas& atlas, ItemId item) {
    if (Items::IsItem(item)) return atlas.GetIndex(ItemData::TextureOf(Items::ItemOf(item)));
    if (Items::IsTool(item)) return ToolTile(atlas, Items::ToolOf(item));
    if (Items::IsArmour(item)) return atlas.GetIndex(Armour::Texture(Items::ArmourOf(item)));
    return IconTile(Items::BlockOf(item));
}
std::string ItemName(ItemId item) {
    if (Items::IsBlock(item)) return BlockName(Items::BlockOf(item));
    return Items::Name(item);
}

// A stack drawn in a slot from lo to hi (the icon inset by `pad`): what it is, what a
// tool's made of (top right: Fe iron, St steel) and how many (bottom right, if asked).
void DrawStackOn(ui::DrawList* draw, const TextureAtlas& atlas, ui::TextureId texture, ui::Vec2 lo, ui::Vec2 hi, float pad, const Stack& st, bool dim,
                 bool showCount) {
    if (st.Empty()) return;
    glm::vec2 uv0, uv1;
    atlas.GetTileUV(ItemTile(atlas, st.item), uv0, uv1);
    draw->AddImage(texture, ui::Vec2(lo.x + pad, lo.y + pad), ui::Vec2(hi.x - pad, hi.y - pad), ui::Vec2(uv0.x, uv0.y), ui::Vec2(uv1.x, uv1.y),
                   dim ? UI_COL32(255, 255, 255, 80) : 0xFFFFFFFFu);
    auto text = [&](ui::Vec2 at, ui::Color colour, const char* t) {
        draw->AddText(ui::Vec2(at.x + 1.0f, at.y + 1.0f), UI_COL32(0, 0, 0, 230), t);
        draw->AddText(at, colour, t);
    };
    if (Items::IsTool(st.item) && Items::TierOf(st.item) > 0) {
        const bool steel = Items::TierOf(st.item) == 2;
        const char* badge = steel ? "St" : "Fe";
        const ui::Vec2 size = ui::CalcTextSize(badge);
        text(ui::Vec2(hi.x - size.x - 3.0f, lo.y + 1.0f), steel ? UI_COL32(150, 220, 255, 255) : UI_COL32(215, 215, 225, 255), badge);
    }
    if (showCount && st.count > 1) {
        const std::string count = std::to_string(st.count);
        const ui::Vec2 size = ui::CalcTextSize(count.c_str());
        text(ui::Vec2(hi.x - size.x - 3.0f, hi.y - size.y - 1.0f), 0xFFFFFFFFu, count.c_str());
    }
}

const char* DefaultBlockName(BlockType type) {
    switch (type) {
    case BlockType::GRASS: return "Grass";
    case BlockType::DIRT:  return "Dirt";
    case BlockType::STONE: return "Stone";
    case BlockType::DEEPSTONE: return "Deepstone";
    case BlockType::GRAVE: return "Grave";
    case BlockType::LAMP:  return "Lamp";
    case BlockType::LOG:   return "Log";
    case BlockType::LEAVES: return "Leaves";
    case BlockType::TALL_GRASS: return "Tall grass";
    case BlockType::FLOWER_RED: return "Red flower";
    case BlockType::FLOWER_YELLOW: return "Yellow flower";
    case BlockType::SAND:  return "Sand";
    case BlockType::WATER: return "Water";
    case BlockType::LAVA: return "Lava";
    case BlockType::OBSIDIAN: return "Obsidian";
    case BlockType::OBSIDIAN_PIPE: return "Obsidian pipe";
    case BlockType::FIRE: return "Fire";
    case BlockType::LAUNCHER: return "Air launcher";
    case BlockType::HAY_BALE: return "Hay bale";
    case BlockType::HEAT_SENSOR: return "Heat sensor";
    case BlockType::SPRINKLER: return "Sprinkler";
    case BlockType::PUMP:  return "Pump";
    case BlockType::PIPE:  return "Pipe";
    case BlockType::WATER_WHEEL: return "Water wheel";
    case BlockType::AIR_PIPE: return "Air pipe";
    case BlockType::DRILL: return "Drill";
    case BlockType::SHAFT: return "Shaft";
    case BlockType::COMPRESSOR: return "Compressor";
    case BlockType::LEVER: return "Lever";
    case BlockType::LEVER_ON: return "Lever (on)";
    case BlockType::LOGIC_GATE: return "Logic gate";
    case BlockType::SIGNAL_LAMP: return "Signal lamp";
    case BlockType::SIGNAL_LAMP_ON: return "Signal lamp (lit)";
    case BlockType::AIR_TANK: return "Air tank";
    case BlockType::CREATIVE_AIR_TANK: return "Creative air tank";
    case BlockType::VALVE: return "Valve (closed)";
    case BlockType::VALVE_OPEN: return "Valve (open)";
    case BlockType::FLUID_VALVE: return "Fluid valve";
    case BlockType::FLUID_VALVE_OPEN: return "Fluid valve (open)";
    case BlockType::PISTON: return "Piston";
    case BlockType::PISTON_HEAD: return "Piston head";
    case BlockType::WATER_SENSOR: return "Water sensor";
    case BlockType::MACHINE_SENSOR: return "Machine sensor";
    case BlockType::BOILER: return "Boiler";
    case BlockType::TURBINE: return "Steam turbine";
    case BlockType::AIR_DIFFUSER: return "Air diffuser";
    case BlockType::COPPER_ORE: return "Copper ore";
    case BlockType::PRESSURE_SENSOR: return "Pressure sensor";
    case BlockType::DAYLIGHT_SENSOR: return "Daylight sensor";
    case BlockType::PLAYER_SENSOR: return "Player sensor";
    case BlockType::ITEM_PIPE: return "Item pipe";
    case BlockType::SMELTER: return "Smelter";
    case BlockType::SMELTER_LIT: return "Smelter (lit)";
    case BlockType::SPRING: return "Spring";
    case BlockType::DRAIN: return "Drain";
    case BlockType::SINKHOLE: return "Sinkhole";
    case BlockType::PRESSURE_GAUGE: return "Pressure gauge";
    case BlockType::OIL_SHALE: return "Oil shale";
    case BlockType::TAR: return "Tar";
    case BlockType::FLUID_PIPE: return "Fluid pipe";
    case BlockType::FLUID_TANK: return "Fluid tank";
    case BlockType::OIL_DERRICK: return "Oil derrick";
    case BlockType::REFINERY: return "Refinery";
    case BlockType::ENGINE: return "Engine";
    case BlockType::FLUID_OUTLET: return "Fluid outlet";
    case BlockType::MINING_POST: return "Mining post";
    case BlockType::STEEL_FRAME: return "Steel frame";
    case BlockType::MACHINE_CASING: return "Machine casing";
    case BlockType::FIRE_BRICK: return "Fire bricks";
    case BlockType::MULTIBLOCK: return "Structure";
    case BlockType::MULTIBLOCK_PART: return "Structure";
    case BlockType::EXTRACTOR: return "Extractor";
    case BlockType::CRUSHER: return "Crusher";
    case BlockType::ASPHALT: return "Asphalt";
    case BlockType::LIMESTONE: return "Limestone";
    case BlockType::CONCRETE: return "Concrete";
    case BlockType::GRAVEL: return "Gravel";
    case BlockType::REINFORCED_CONCRETE: return "Reinforced concrete";
    case BlockType::GLOW_FUNGUS: return "Glow fungus";
    case BlockType::MAGMA: return "Magma";
    case BlockType::GOLD_ORE: return "Gold ore";
    case BlockType::FILTER_PIPE: return "Filter pipe";
    case BlockType::CRAFTER: return "Crafter";
    case BlockType::HARVESTER: return "Harvester";
    case BlockType::STONE_BRICK: return "Stone bricks";
    case BlockType::MOSSY_BRICK: return "Mossy bricks";
    case BlockType::SPIKES: return "Spikes";
    case BlockType::CRUMBLING_BRICK: return "Crumbling bricks";
    case BlockType::PRESSURE_PLATE: return "Pressure plate";
    case BlockType::SLUICE: return "Sluice gate";
    case BlockType::SNOW: return "Snow";
    case BlockType::DRY_GRASS: return "Dry grass";
    case BlockType::LUSH_GRASS: return "Lush grass";
    case BlockType::SNOWY_GRASS: return "Snowy grass";
    case BlockType::PINE_LEAVES: return "Pine needles";
    case BlockType::CACTUS: return "Cactus";
    case BlockType::DEAD_BUSH: return "Dead bush";
    case BlockType::ICE: return "Ice";
    case BlockType::PEBBLES: return "Pebbles";
    case BlockType::PLANKS: return "Planks";
    case BlockType::GLASS: return "Glass";
    case BlockType::CHEST: return "Chest";
    case BlockType::BED: return "Bed";
    case BlockType::COAL_ORE: return "Coal ore";
    case BlockType::IRON_ORE: return "Iron ore";
    case BlockType::CROP: return "Young crop";
    case BlockType::RIPE_WHEAT: return "Ripe wheat";
    case BlockType::LADDER: return "Ladder";
    case BlockType::DOOR: return "Door";
    case BlockType::DOOR_OPEN: return "Open door";
    case BlockType::CAMPFIRE: return "Campfire";
    case BlockType::SIGN: return "Sign";
    case BlockType::TNT: return "TNT";
    case BlockType::TNT_LIT: return "Lit TNT";
    case BlockType::SAPLING: return "Sapling";
    case BlockType::PINE_SAPLING: return "Pine sapling";
    default:               return "Air";
    }
}

// Saves the default framebuffer's back buffer (the frame about to be shown) as a PNG.
void SaveScreenshot(const std::string& path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1);
    if (stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3)) std::cout << "Saved " << path << std::endl;
    else std::cerr << "Could not save " << path << std::endl;
}

void GlfwErrorCallback(int error, const char* description) {
    std::cerr << "GLFW error " << error << ": " << description << std::endl;
}

} // namespace

Application::Application() = default;

Application::~Application() {
    Shutdown();
}

bool Application::Init(const ClientOptions& options) {
    settings.Load(kSettingsFile);
    // The first time the game is opened (no name yet, or the old default): a name of
    // its own, kept from now on (servers won't take two players of one name).
    if (settings.name.empty() || settings.name == "Player") {
        settings.name = PlayerNames::Random();
        settings.Save(kSettingsFile);
    }
    vsync = settings.vsync;
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        std::cerr << "ERROR: Failed to initialize GLFW" << std::endl;
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);

    window = glfwCreateWindow(kWindowWidth, kWindowHeight, "VoxelEngine", nullptr, nullptr);
    if (!window) {
        std::cerr << "ERROR: Failed to create window" << std::endl;
        return false;
    }

    glfwMakeContextCurrent(window);
    if (!gladLoadGL()) {
        std::cerr << "ERROR: Failed to initialize GLAD" << std::endl;
        return false;
    }

    glfwSwapInterval(vsync ? 1 : 0);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // For unattended checks: VOXEL_AUTO_SCREENSHOT="seconds:path.png" saves one
    // screenshot that long after start, and VOXEL_START_YAW sets the initial heading.
    if (const char* shot = std::getenv("VOXEL_AUTO_SCREENSHOT")) {
        const std::string spec = shot;
        const size_t colon = spec.find(':');
        if (colon != std::string::npos) {
            autoScreenshotAt = std::atof(spec.substr(0, colon).c_str());
            autoScreenshotPath = spec.substr(colon + 1);
        }
    }
    // VOXEL_NO_MOUSE=1 ignores mouse look, so automated screenshots from a saved
    // position always face the same way.
    ignoreMouse = std::getenv("VOXEL_NO_MOUSE") != nullptr;
    // VOXEL_HOLD_MINE=1 mines whatever's in view, as if the left button were held
    // (for unattended screenshots of mining).
    holdMine = std::getenv("VOXEL_HOLD_MINE") != nullptr;
    // VOXEL_AUTO_FLY=1 flies straight ahead, sprinting, in creative (for watching how
    // the world streams in as you travel).
    autoFly = std::getenv("VOXEL_AUTO_FLY") != nullptr;
    // VOXEL_RUN="/time noon;/summon deer" sends those chat lines (commands) once joined.
    if (const char* run = std::getenv("VOXEL_RUN")) startCommands = run;
    // VOXEL_BENCH=1 turns vsync off and prints where frame time goes, once a second.
    if (std::getenv("VOXEL_BENCH")) {
        bench = true;
        vsync = false;
        glfwSwapInterval(0);
    }
    if (const char* yaw = std::getenv("VOXEL_START_YAW")) camera.SetYaw(static_cast<float>(std::atof(yaw)));
    // VOXEL_START_PITCH sets the initial pitch (degrees), and VOXEL_RIGHT_CLICK="seconds"
    // right-clicks once that long after start (for unattended checks of building).
    if (const char* pitch = std::getenv("VOXEL_START_PITCH")) camera.SetLook(camera.GetDirectionAngles().y, static_cast<float>(std::atof(pitch)));
    if (const char* click = std::getenv("VOXEL_RIGHT_CLICK")) rightClickAt = std::atof(click);

    // The window's input goes to the game and to its UI (src/ui/Ui.h).
    glfwSetWindowUserPointer(window, this);
    glfwSetKeyCallback(window, KeyCallback);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorPosCallback);
    glfwSetScrollCallback(window, ScrollCallback);
    glfwSetCharCallback(window, CharCallback);

    {
        std::string error;
        if (!ui::Init("Resources/Fonts", error)) {
            std::cerr << "ERROR: the UI couldn't start: " << error << std::endl;
            return false;
        }
        ui::SetClipboardGetter([](void* w) { return glfwGetClipboardString(static_cast<GLFWwindow*>(w)); }, window);
        uiInitialized = true;
    }

    // On high-DPI displays the framebuffer is larger than the window.
    int framebufferWidth, framebufferHeight;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    renderer = std::make_unique<Renderer>();
    if (!renderer->Init(framebufferWidth, framebufferHeight)) return false;
    // Sound (silent if there's no audio device). VOXEL_MUTE=1 keeps it quiet.
    mute = std::getenv("VOXEL_MUTE") != nullptr;
    audio = std::make_unique<Audio>();
    if (!mute) audio->Init("Resources/Sounds");

    // Block textures must be resolved before the world starts meshing.
    LoadData();
    LoadBlockTextures();
    LoadHotbar();
    LoadChatHistory();
    // VOXEL_OPEN_INVENTORY=1 starts with the inventory open (for screenshots).
    if (std::getenv("VOXEL_OPEN_INVENTORY")) SetInventoryOpen(true);
    // VOXEL_ITEM_INFO="iron_ingot": the item panel starts out describing that (for unattended checks).
    if (const char* info = std::getenv("VOXEL_ITEM_INFO")) itemInfo = Items::FromId(info);
    ApplySettings();
    if (options.showMenu) {
        // The title screen first: nothing to connect to yet.
        SetCursorCaptured(false);
        RefreshWorlds();
        std::snprintf(serverAddress, sizeof serverAddress, "%s", settings.lastServer.c_str());
        // VOXEL_MENU_PAGE=worlds|new|multiplayer|settings opens that page (for screenshots).
        if (const char* p = std::getenv("VOXEL_MENU_PAGE")) {
            const std::string name = p;
            page = name == "worlds" ? Page::Worlds : name == "new" ? Page::NewWorld : name == "multiplayer" ? Page::Multiplayer
                 : name == "settings" || name == "controls" ? Page::Settings : Page::Title;
        }
        return true;
    }
    ClientOptions start = options;
    if (!start.nameGiven) start.name = settings.name;
    return StartGame(start);
}

bool Application::StartGame(const ClientOptions& options) {
    menuMessage.clear();
    lanBrowser.reset(); // (no more looking for games: one's starting)
    worldName = std::filesystem::path(options.worldDirectory).filename().string();
    world = std::make_unique<ClientWorld>();
    client = std::make_unique<Client>(*world);

    // Singleplayer runs a server in this process and connects to it like any other
    // client, over the loopback interface on a free port.
    std::string host = options.host;
    uint16_t port = options.port ? options.port : net::kDefaultPort;
    if (host.empty()) {
        localServer = std::make_unique<Server>();
        Server::Options serverOptions;
        serverOptions.bindHost = "127.0.0.1";
        serverOptions.port = options.port; // 0: any free port
        serverOptions.worldDirectory = options.worldDirectory;
        serverOptions.waterSimulationDistance = options.waterSimulationDistance;
        if (options.emptyWorld) serverOptions.worldType = TerrainGenerator::Type::Empty;
        serverOptions.dayLengthSeconds = options.dayLengthSeconds;
        serverOptions.viewDistance = settings.renderDistance; // no further than we'll look
        serverOptions.ownerName = options.name; // it's your world: you're its operator
        serverOptions.weather = static_cast<Server::Options::Weather>(options.weather);
        serverOptions.defaultMode = options.mode >= 0 ? static_cast<net::GameMode>(options.mode)
                                    : options.emptyWorld ? net::GameMode::Creative : net::GameMode::Survival;
        if (!localServer->Start(serverOptions)) {
            menuMessage = "Couldn't open the world in " + options.worldDirectory;
            StopGame();
            return false;
        }
        localServer->RunInBackground();
        host = "127.0.0.1";
        port = localServer->GetPort();
        serverLabel = "singleplayer";
        mapCacheFile = options.worldDirectory.empty() ? std::string() : options.worldDirectory + "/map_cache.bin"; // (--no-save: no map kept either)
    }
    else if (!options.joinCode.empty()) {
        serverLabel = "a friend's game (" + options.joinCode + ")";
        mapCacheFile.clear(); // (a fresh code each time it's opened: nothing to match a map to)
    }
    else {
        serverLabel = host + ":" + std::to_string(port);
        std::string safe = host + "_" + std::to_string(port);
        for (char& c : safe)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') c = '_';
        mapCacheFile = "maps/" + safe + ".bin";
    }
    LoadMapCache();

    std::string error;
    if (!client->Connect(host, port, options.name, error, settings.renderDistance, options.joinCode)) {
        std::cerr << "ERROR: Could not connect to " << host << ":" << port << ": " << error << std::endl;
        menuMessage = options.joinCode.empty() ? "Couldn't connect to " + host + ":" + std::to_string(port) + ": " + error
                                               : "Couldn't join " + options.joinCode + ": " + error;
        StopGame();
        return false;
    }
    paused = false;
    SetCursorCaptured(true);
    // VOXEL_RELAY=host:port and VOXEL_OPEN_TO_FRIENDS=1 open singleplayer to friends at
    // once, through that relay (for screenshots and trying it out).
    if (const char* relayAddress = std::getenv("VOXEL_RELAY"); relayAddress && localServer && std::getenv("VOXEL_OPEN_TO_FRIENDS")) {
        relayHost = std::make_unique<net::RelayHost>();
        if (!relayHost->Start(relayAddress, localServer->GetPort(), relayError)) relayHost.reset();
    }
    // VOXEL_PAUSE=1 starts with the pause menu open (for screenshots).
    if (std::getenv("VOXEL_PAUSE")) {
        paused = true;
        SetCursorCaptured(false);
    }
    return true;
}

void Application::StopGame() {
    SaveMapCache();
    mapCache.clear();
    mapCacheFile.clear();
    if (mapTexture) {
        glDeleteTextures(1, &mapTexture);
        mapTexture = 0; // (a fresh picture for the next game)
    }
    if (client) client->Disconnect();
    client.reset();
    // The world's chunks are freed in the background: on Windows that alone takes over a
    // second with a full view, and the menu needn't wait for it.
    if (world) {
        if (freeingWorld.valid()) freeingWorld.wait();
        freeingWorld = std::async(std::launch::async, [chunks = world->Release()]() mutable { chunks.clear(); });
    }
    world.reset();
    relayHost.reset(); // (closes the game to friends)
    relayError.clear();
    lanHost.reset();
    lanError.clear();
    if (localServer) localServer->Stop(); // (saves the world)
    localServer.reset();
    joined = false;
    mining = false;
    walkers.clear();
    creatureSounds.clear();
    if (audio) {
        audio->SetLoop("rain_loop", 0.0f);
        audio->SetHums({}); // (machines' hums stop with the world)
    }
    paused = pauseSettings = false;
    chatOpen = inventoryOpen = false;
    // (Per game: the next one starts afresh.)
    hotbarSeeded = false;
    chatSeen = 0;
    rebinding = -1;
    landedAt = 0.0;
    player = Player(glm::vec3(7.0f, 7.0f, 7.0f));
    camera = Camera();
    ApplySettings();
    SetCursorCaptured(false);
}

void Application::SetCursorCaptured(bool captured) {
    captureCursor = captured;
    if (window) glfwSetInputMode(window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    camera.ResetLook();
}

void Application::ApplySettings() {
    settings.Clamp();
    camera.SetFov(settings.fov);
    camera.SetSensitivity(settings.sensitivity);
    if (!bench) vsync = settings.vsync;
}



void Application::RunMenuFrame(double now) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glClearColor(0.42f, 0.62f, 0.86f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ui::GetIO().ConfigFlags &= ~(ui::ConfigFlags_NoMouse | ui::ConfigFlags_NoKeyboard); // the menus type and click
    BeginUiFrame();
    DrawMenu();
    EndUiFrame();
    if (!client && !autoScreenshotPath.empty() && now >= autoScreenshotAt) {
        SaveScreenshot(autoScreenshotPath, width, height);
        autoScreenshotPath.clear();
    }
    glfwSwapInterval(vsync ? 1 : 0);
    glfwSwapBuffers(window);
}

namespace {
uint32_t DefaultMapColour(BlockType type); // (below, with the minimap)
}

bool DumpBlockData(const std::string& path) {
    std::ofstream out(path);
    if (!out) return false;
    auto hex = [](uint32_t abgr) {
        char text[8];
        std::snprintf(text, sizeof text, "#%02x%02x%02x", abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF);
        return std::string(text);
    };
    auto tool = [](Tool t) {
        switch (t) {
        case Tool::Pickaxe: return "pickaxe";
        case Tool::Axe: return "axe";
        case Tool::Shovel: return "shovel";
        case Tool::Sword: return "sword";
        default: return "none";
        }
    };
    out << "// Blocks' details, by block id. Change any of them (or leave a block out, or a\n"
           "// field out, for the game's own). Press R in the game to load this file again.\n"
           "//   \"name\": what the game calls it\n"
           "//   \"textures\": {\"all\": x}, or {\"top\", \"bottom\", \"side\"}, or each of \"top\", \"bottom\",\n"
           "//       \"front\", \"back\", \"left\" and \"right\": PNGs in Resources/Textures/Blocks\n"
           "//   \"hardness\": seconds to break by hand    \"tool\": pickaxe, axe, shovel, sword or none\n"
           "//   \"sound\": soft, hard, sand, wood or glass  \"light\": 0 to 15\n"
           "//   \"map\": its colour on the minimap\n"
           "{\n";
    for (int i = 0; i < static_cast<int>(BlockType::COUNT); i++) {
        const BlockType t = static_cast<BlockType>(i);
        std::string faces[6];
        for (int f = 0; f < 6; f++) faces[f] = DefaultBlockTexture(t, f) ? DefaultBlockTexture(t, f) : "";
        std::string textures;
        auto q = [](const std::string& s) { return "\"" + s + "\""; };
        if (faces[0].empty()) textures = "";
        else if (std::all_of(faces, faces + 6, [&](const std::string& f) { return f == faces[0]; }) || faces[1].empty())
            textures = "{ \"all\": " + q(faces[0]) + " }";
        else if (faces[2] == faces[3] && faces[2] == faces[4] && faces[2] == faces[5])
            textures = "{ \"top\": " + q(faces[0]) + ", \"bottom\": " + q(faces[1]) + ", \"side\": " + q(faces[2]) + " }";
        else
            textures = "{ \"top\": " + q(faces[0]) + ", \"bottom\": " + q(faces[1]) + ", \"front\": " + q(faces[2]) + ", \"back\": " + q(faces[3]) +
                       ", \"left\": " + q(faces[4]) + ", \"right\": " + q(faces[5]) + " }";
        char hardness[16];
        std::snprintf(hardness, sizeof hardness, "%g", Mining::DefaultHardness(t));
        out << "  " << q(i == 0 ? "air" : BlockId(t)) << ": { \"name\": " << q(DefaultBlockName(t)) << (textures.empty() ? "" : ", \"textures\": " + textures)
            << ", \"hardness\": " << hardness << ", \"tool\": " << q(tool(Mining::DefaultBestTool(t))) << ", \"sound\": " << q(Audio::Material(t))
            << ", \"light\": " << int(DefaultLightEmission(t)) << ", \"map\": " << q(hex(DefaultMapColour(t))) << " }"
            << (i + 1 < static_cast<int>(BlockType::COUNT) ? "," : "") << "\n";
    }
    out << "}\n";
    return static_cast<bool>(out);
}

void Application::LoadData() {
    // The editable data files (Resources/Data): blocks' and items' details and creatures' models.
    std::string error;
    if (!BlockData::Load(BlockData::kPath, error)) std::cerr << "[data] " << error << " (using the game's own block details)" << std::endl;
    if (!ItemData::Load(ItemData::kPath, error)) std::cerr << "[data] " << error << " (using the game's own item details)" << std::endl;
    if (std::filesystem::exists(Bodies::kPath) && !Bodies::Load(Bodies::kPath, error)) std::cerr << "[data] " << error << " (the game's own body sizes)" << std::endl;
    if (!renderer->LoadCreatureModels(error)) std::cerr << "[data] " << error << " (creatures drawn as plain boxes)" << std::endl;
    if (!renderer->LoadStructureModels(error)) std::cerr << "[data] " << error << " (structures not drawn)" << std::endl;
    if (std::filesystem::exists(Crafting::kRecipesPath) && !Crafting::Load(Crafting::kRecipesPath, error)) std::cerr << "[data] " << error << " (the game's own recipes)" << std::endl;
    if (std::filesystem::exists(Smelting::kPath) && !Smelting::Load(Smelting::kPath, error)) std::cerr << "[data] " << error << " (the game's own smelting)" << std::endl;
}

void Application::WatchData(double now) {
    if (now - dataCheckedAt < 1.0) return;
    dataCheckedAt = now;
    std::vector<std::string> changed;
    for (const char* path : { BlockData::kPath, ItemData::kPath, Bodies::kPath, PlayerModels::kCreatureModelsPath, PlayerModels::kStructureModelsPath, Crafting::kRecipesPath, Smelting::kPath }) {
        const std::string stamp = Json::DataStamp(path);
        auto it = dataTimes.find(path);
        if (it == dataTimes.end()) dataTimes[path] = stamp; // (the first look: as loaded)
        else if (it->second != stamp) {
            it->second = stamp;
            changed.push_back(path);
        }
    }
    if (changed.empty()) return;
    std::string error;
    for (const auto& path : changed) {
        const std::string name = std::filesystem::path(path).filename().string();
        bool ok = true;
        if (path == BlockData::kPath) {
            ok = BlockData::Load(BlockData::kPath, error);
            LoadBlockTextures();
            world->RebuildAllChunks();
        } else if (path == ItemData::kPath)
            ok = ItemData::Load(ItemData::kPath, error);
        else if (path == Bodies::kPath)
            ok = Bodies::Load(Bodies::kPath, error);
        else if (path == Crafting::kRecipesPath)
            ok = Crafting::Load(Crafting::kRecipesPath, error);
        else if (path == Smelting::kPath)
            ok = Smelting::Load(Smelting::kPath, error);
        else if (path == PlayerModels::kCreatureModelsPath)
            ok = renderer->LoadCreatureModels(error);
        else
            ok = renderer->LoadStructureModels(error);
        if (client) {
            if (ok) client->Note("Loaded " + name + " again");
            else client->Note(error, net::ChatMessage::Kind::Error);
        }
        if (!ok) std::cerr << "[data] " << error << std::endl;
    }
}

void Application::LoadBlockTextures() {
    const TextureAtlas& atlas = renderer->GetAtlas();
    ResolveBlockTextures([&](const std::string& name) {
        int index = atlas.GetIndex(name);
        if (index == TextureAtlas::kMissingIndex)
            std::cerr << "Missing block texture '" << name << "'" << std::endl;
        return index;
    });
}

void Application::Shutdown() {
    SaveMapCache();
    // Order matters: everything holding GL objects goes before the context.
    if (mapTexture) {
        glDeleteTextures(1, &mapTexture);
        mapTexture = 0;
    }
    audio.reset();
    if (client) client->Disconnect();
    client.reset();
    // Quitting: the world's chunks aren't freed one by one (over a second on Windows with
    // a full view); the process ends straight after and the system takes the memory back
    // all at once. Their GPU objects go now, while the context is still here.
    if (freeingWorld.valid()) freeingWorld.wait(); // (a world left earlier, still being freed)
    if (world) new ClientWorld::ChunkMap(world->Release()); // (deliberately never freed)
    world.reset();
    renderer.reset();
    relayHost.reset();
    lanHost.reset();
    lanBrowser.reset();
    if (localServer) localServer->Stop();
    localServer.reset();

    if (uiInitialized) {
        ui::Shutdown();
        uiInitialized = false;
    }

    if (window) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

void Application::Run() {
    double lastFrame = glfwGetTime();
    bool appliedVsync = vsync;

    while (!glfwWindowShouldClose(window)) {
        const double now = glfwGetTime();
        const float deltaTime = static_cast<float>(now - lastFrame);
        lastFrame = now;

        glfwPollEvents();
        if (!client) {
            RunMenuFrame(now); // the title screen
            continue;
        }

        double mark = glfwGetTime();
        const double frameStart = mark;
        for (double& s : benchFrame) s = 0.0;
        auto lap = [&](int section) {
            if (!bench) return;
            const double t = glfwGetTime();
            benchSections[section] += t - mark;
            benchFrame[section] += t - mark;
            mark = t;
        };
        UpdateNetwork();
        WatchData(now);
        lap(0);
        if (rightClickAt > 0.0 && glfwGetTime() >= rightClickAt) {
            rightClickAt = 0.0;
            OnMouseButton(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
        }
        HandleContinuousInput(deltaTime);
        HandleGamepad(deltaTime);
        {
            // A gentle bob while walking on the ground (eased in and out), and the view
            // widening a few degrees while sprinting.
            const glm::vec3 v = player.GetVelocity();
            const float speed = glm::length(glm::vec2(v.x, v.z));
            const float dt = static_cast<float>(deltaTime);
            const bool walking = player.IsOnGround() && !player.IsFlying() && speed > 0.5f && settings.viewBobbing;
            bobAmount += ((walking ? std::min(1.0f, speed / 4.5f) : 0.0f) - bobAmount) * std::min(1.0f, dt * 8.0f);
            bobPhase += speed * dt * 1.9f;
            const bool sprinting = speed > 5.5f && !player.IsFlying();
            fovBoost += ((sprinting ? 8.0f : 0.0f) - fovBoost) * std::min(1.0f, dt * 6.0f);
            camera.SetFov(settings.fov + fovBoost);
            const float yaw = glm::radians(camera.GetYaw());
            const glm::vec3 side(-std::sin(yaw), 0.0f, std::cos(yaw));
            const glm::vec3 bob = glm::vec3(0.0f, std::abs(std::sin(bobPhase)) * 0.06f, 0.0f) + side * std::sin(bobPhase) * 0.035f;
            const float t = static_cast<float>(glfwGetTime());
            const glm::vec3 jolt = glm::vec3(std::sin(t * 47.0f), std::sin(t * 53.0f) * 0.6f, std::cos(t * 41.0f)) * 0.12f * shake * shake;
            camera.SetPosition(player.GetEyePosition() + bob * bobAmount + jolt);
        }
        world->Update(player.GetFeetPosition() + glm::vec3(0.0f, 0.1f, 0.0f)); // (the chunk the server streams round: the feet's, not the eye's)
        lap(1);

        FrameParams params;
        params.view = camera.GetViewMatrix();
        params.projection = camera.GetProjectionMatrix(static_cast<float>(renderer->GetWidth()) / renderer->GetHeight());
        params.time = static_cast<float>(now);
        params.wireframe = wireframe;
        params.ambientOcclusion = ambientOcclusion;
        params.cameraPosition = camera.GetPosition();
        params.underwater = world->IsUnderwater(camera.GetPosition());
        params.fogEnd = static_cast<float>(world->GetViewDistance() * Chunk::CHUNK_SIZE);
        UpdateSky(deltaTime, params);
        UpdateSounds();
        UpdateParticles(static_cast<float>(deltaTime));
        UpdateLightning(static_cast<float>(deltaTime));
        UpdateFireflies(static_cast<float>(deltaTime));
        if (auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), Reach())) {
            params.highlightedBlock = hit->block;
            Block target;
            if (world->GetBlock(hit->block.x, hit->block.y, hit->block.z, target)) params.highlightOutline = target.OutlineSegments(hit->block, ModelBoxes::Joins(*world, hit->block, target));
        }
        // With the chisel: the quarter it would cut (or build up) picked out in gold.
        params.chiselOutline.clear();
        if (const auto aim = AimChisel(); aim && params.highlightedBlock == aim->block) params.chiselOutline = aim->target.QuarterOutline(aim->shapeDir, aim->edge);
        if (mining && mineProgress > 0.0f) {
            // Cracks over the box the block's shape fills.
            glm::vec3 lo(1.0f), hi(0.0f);
            for (const glm::vec3& p : miningBlock.OutlineSegments(miningPos)) lo = glm::min(lo, p), hi = glm::max(hi, p);
            if (hi.x > lo.x) {
                params.breakProgress = mineProgress;
                params.breakBoxMin = glm::vec3(miningPos) + lo;
                params.breakBoxSize = hi - lo;
            }
        }
        // Light carried in hand: ours (a lamp or campfire selected, and some to hold), and
        // other players'.
        {
            const BlockType mine = SelectedTool() == Tool::None ? SelectedBlock() : BlockType::AIR;
            if (GetLightEmission(mine) > 0 && (client->IsCreative() || client->GetResources(mine) > 0))
                params.handLights.push_back(glm::vec4(player.GetEyePosition() - glm::vec3(0.0f, 0.4f, 0.0f), static_cast<float>(GetLightEmission(mine))));
            for (const auto& other : client->GetRemotePlayers())
                if (const BlockType held = Items::BlockOf(other.heldItem); GetLightEmission(held) > 0)
                    params.handLights.push_back(glm::vec4(other.feet + glm::vec3(0.0f, 1.2f, 0.0f), static_cast<float>(GetLightEmission(held))));
        }
        // Figures are lit by the light where they stand: the brighter of their feet's
        // cell (which may hold a slope or a plant) and the one above.
        auto lightAt = [&](const glm::vec3& feet) {
            glm::vec2 best(0.0f);
            bool any = false;
            for (int dy = 0; dy <= 1; dy++) {
                int blockLight = 0, skyLight = 0;
                const glm::ivec3 cell(glm::floor(feet + glm::vec3(0.0f, 0.1f + dy, 0.0f)));
                Block b;
                if (!world->GetLight(cell.x, cell.y, cell.z, blockLight, skyLight)) continue;
                if (world->GetBlock(cell.x, cell.y, cell.z, b) && b.IsFullBlock()) continue; // (inside a wall: no light there)
                best = glm::max(best, glm::vec2(skyLight, blockLight));
                any = true;
            }
            if (!any) best = glm::vec2(15.0f, 0.0f);
            // (And by lights carried nearby.)
            for (const auto& h : params.handLights) best.y = std::max(best.y, h.w - glm::distance(feet + glm::vec3(0.0f, 0.9f, 0.0f), glm::vec3(h)));
            return best;
        };
        for (const auto& c : client->GetCreatures())
        {
            // When each creature's action began (its animation plays from then).
            CreatureAction& a = creatureActions[c.id];
            if (c.action != a.action) a.action = c.action, a.since = now;
            a.seen = now;
            params.creatures.push_back({ static_cast<uint8_t>(c.type), c.feet, c.yaw, c.hurt, c.speed, lightAt(c.feet), c.tame, c.action,
                                         static_cast<float>(now - a.since) });
        }
        for (auto it = creatureActions.begin(); it != creatureActions.end();) it = now - it->second.seen > 5.0 ? creatureActions.erase(it) : std::next(it);
        for (const auto& b : bolts) params.bolts.push_back({ b.points, b.life / 0.35f });
        for (const auto& f : fireflies) {
            // Each blinks in its own rhythm, and fades in and out at the ends of its life.
            const float blink = std::max(0.0f, std::sin(f.phase * 2.3f + f.home.x * 7.0f));
            params.glows.push_back({ f.position, 0.06f, blink * std::min(1.0f, f.life * 0.5f) });
        }
        // Our bobber, floating on the water and bobbing; a splash when a fish bites.
        if (client->TakeBite() && lastBobber) {
            SpawnParticles(*lastBobber, BlockType::WATER, 14, 2.0f);
            PlayAt("splash", *lastBobber, 0.8f);
        }
        lastBobber = client->GetBobber();
        if (lastBobber) {
            Block water;
            float surface = 0.9f;
            if (world->GetBlock(lastBobber->x, lastBobber->y, lastBobber->z, water)) surface = water.WaterShownLevel() / static_cast<float>(Block::kMaxWaterVolume);
            const glm::vec3 at = glm::vec3(*lastBobber) + glm::vec3(0.5f, surface + 0.04f * std::sin(static_cast<float>(Client::Now()) * 3.0f), 0.5f);
            glm::vec2 uv0, uv1;
            renderer->GetAtlas().GetTileUV(renderer->GetAtlas().GetIndex("bobber"), uv0, uv1);
            params.particles.push_back({ at, 0.18f, glm::vec4(uv0, uv1), lightAt(at), 0.0f, true });
        }
        for (const auto& a : client->GetArrows()) {
            // Flying arrows point where they're going; stuck ones keep the way they came in.
            glm::vec3 dir = a.velocity;
            if (glm::length(dir) < 1e-3f) {
                auto& kept = arrowFacing[std::make_pair(static_cast<int>(a.position.x * 64.0f), static_cast<int>(a.position.z * 64.0f))];
                dir = glm::length(kept) > 0.0f ? kept : glm::vec3(0.0f, -1.0f, 0.0f);
            }
            else arrowFacing[std::make_pair(static_cast<int>(a.position.x * 64.0f), static_cast<int>(a.position.z * 64.0f))] = dir;
            params.arrows.push_back({ a.position, dir, lightAt(a.position) });
        }
        if (arrowFacing.size() > 512) arrowFacing.clear();
        {
            const TextureAtlas& atlas = renderer->GetAtlas();
            for (const auto& i : client->GetItems()) {
                if (!Items::Valid(i.item)) continue;
                ItemModel model;
                model.position = i.position;
                const BlockType type = Items::BlockOf(i.item);
                model.cube = Items::IsBlock(i.item) && !IsPlantType(type) && !Block(type).IsLiquid();
                glm::vec2 uv0, uv1;
                atlas.GetTileUV(ItemTile(atlas, i.item), uv0, uv1);
                model.uv = glm::vec4(uv0, uv1);
                model.copies = i.count >= 32 ? 3 : i.count >= 2 ? 2 : 1;
                model.phase = static_cast<float>(i.id % 97) * 0.37f;
                model.light = lightAt(i.position);
                params.items.push_back(model);
            }
            const glm::vec3 chest = player.GetEyePosition() - glm::vec3(0.0f, 0.6f, 0.0f);
            bool picked = false;
            for (const glm::vec3& at : client->TakeVanishedItems()) picked |= glm::distance(at, chest) < 2.0f;
            if (picked) audio->PlayHere("pickup", 0.35f);
        }
        UpdateMachines(params, lightAt);
        for (const auto& p : particles) {
            // Bits shrink away at the end; spray clouds billow out to two and a half times
            // their size, and shrink away only right at the end.
            const float age = 1.0f - p.life / p.maxLife;
            const float size = p.mist ? p.size * (1.0f + 1.5f * age) * std::min(1.0f, p.life / (0.25f * p.maxLife))
                                      : p.size * std::min(1.0f, p.life / (0.4f * p.maxLife));
            // Clouds of spray and smoke are flat billboards; bits of blocks and drops are cubes.
            params.particles.push_back({ p.position, size, p.uv, lightAt(p.position - glm::vec3(0.0f, 0.1f, 0.0f)), p.mist ? 1.0f : 0.0f, p.mist || p.rise || p.flame,
                                         p.flame ? 1.0f : 0.0f });
        }
        {
            // Steam: a translucent cloud in the cells it fills (render/SteamVolume.h).
            const glm::vec3 eye = camera.GetPosition();
            const auto& steam = client->GetSteam();
            long long near = 0;
            glm::ivec3 thickest(0);
            int most = -1;
            params.steamCells.clear();
            params.steamCells.reserve(steam.size());
            for (const auto& s : steam) {
                if (glm::distance(glm::vec3(s.cell) + glm::vec3(0.5f), eye) < 24.0f) near += s.density;
                if (s.density > most) most = s.density, thickest = s.cell;
                params.steamCells.emplace_back(glm::vec3(s.cell), static_cast<float>(s.density));
            }
            params.steamVersion = client->GetSteamVersion();
            // With our head in it, the steam whites the view out (thicker steam, more).
            {
                const glm::ivec3 head(glm::floor(eye));
                int here = 0;
                for (const auto& s : steam)
                    if (s.cell == head) here = s.density;
                const float target = std::clamp(here / 160.0f, 0.0f, 1.0f);
                steamFog += (target - steamFog) * std::min(1.0f, deltaTime * 5.0f);
                params.steamFog = steamFog;
            }
            // On fire: flames round the screen (fading in fast, out slower), and crackling.
            {
                const bool alight = client->Burning();
                burnGlow += ((alight ? 1.0f : 0.0f) - burnGlow) * std::min(1.0f, deltaTime * (alight ? 8.0f : 2.5f));
                params.burning = burnGlow;
                if (alight && Client::Now() >= nextBurnCrackle) {
                    audio->Play("fire_crackle", camera.GetPosition(), 0.8f, 0.2f);
                    nextBurnCrackle = Client::Now() + 0.45;
                }
            }
            // A burst near us hisses.
            if (client->GetSteamVersion() != steamSeen) {
                if (near > steamNear + 400 && most >= 0) PlayAt("hiss", thickest, 0.8f);
                steamSeen = client->GetSteamVersion();
                steamNear = near;
            }
        }
        const TextureAtlas& atlas = renderer->GetAtlas();
        for (const auto& other : client->GetRemotePlayers()) {
            PlayerModel model{ other.feet, other.yaw, other.pitch, PlayerModels::ColorFor(other.id), (other.flags & net::kFlagHurt) != 0, lightAt(other.feet) };
            model.crouching = (other.flags & net::kFlagCrouching) != 0;
            model.boat = (other.flags & net::kFlagBoat) != 0;
            // What's in their hand: a tool's or an item's icon as a card, a block as a
            // little cube.
            int tile = -1;
            if (other.heldTool != Tool::None) tile = ToolTile(atlas, other.heldTool), model.held = PlayerModel::Held::Icon;
            else if (other.heldItem != Items::kNone) {
                tile = ItemTile(atlas, other.heldItem);
                const BlockType held = Items::BlockOf(other.heldItem);
                const bool flat = !Items::IsBlock(other.heldItem) || IsPlantType(held) || Block(held).IsLiquid();
                model.held = flat ? PlayerModel::Held::Icon : PlayerModel::Held::Block;
            }
            if (tile >= 0) {
                glm::vec2 uv0, uv1;
                atlas.GetTileUV(tile, uv0, uv1);
                model.heldUV = glm::vec4(uv0, uv1);
            }
            model.armour = other.armour;
            params.players.push_back(model);
        }

        lap(2);
        renderer->RenderFrame(*world, params);
        lap(3);
        if (bench) glFinish(); // wait for the GPU, so its time shows up as its own section
        lap(6);

        // With the cursor captured it's hidden but still moves; keep it from clicking
        // on the UI by accident.
        ui::IO& io = ui::GetIO();
        if (captureCursor) io.ConfigFlags |= ui::ConfigFlags_NoMouse;
        else io.ConfigFlags &= ~ui::ConfigFlags_NoMouse;
        // The keyboard is the UI's only while something takes typing (chat, the pause
        // menu's settings, the inventory); otherwise it's the game's.
        if (chatOpen || paused || inventoryOpen) io.ConfigFlags &= ~ui::ConfigFlags_NoKeyboard;
        else io.ConfigFlags |= ui::ConfigFlags_NoKeyboard;

        BeginUiFrame();
        DrawNameTags(params.projection * params.view);
        DrawSigns(params.projection * params.view);
        if (showCompass) DrawCompass();
        if (showMap) DrawMinimap();
        DrawToast();
        DrawBossBar();
        DrawDeath();
        if (showPlayers) DrawPlayerList();
        DrawHotbar();
        DrawStatus();
        DrawChat();
        DrawCrosshair();
        DrawHealth();
        DrawInventory();
        if (showDebug) DrawDebugWindow(deltaTime);
        // Dropped by the server: nothing to do but go back to the title screen.
        if (!client->IsConnected() && !client->GetDisconnectReason().empty() && !paused) {
            paused = true;
            SetCursorCaptured(false);
        }
        if (paused) DrawPauseMenu();
        EndUiFrame();
        lap(4);

        if (screenshotRequested || (!autoScreenshotPath.empty() && now >= autoScreenshotAt)) {
            std::string path = autoScreenshotPath;
            if (screenshotRequested) {
                char name[64];
                const std::time_t t = std::time(nullptr);
                std::strftime(name, sizeof(name), "screenshot-%Y%m%d-%H%M%S.png", std::localtime(&t));
                path = name;
            }
            SaveScreenshot(path, renderer->GetWidth(), renderer->GetHeight());
            if (!screenshotRequested) autoScreenshotPath.clear();
            screenshotRequested = false;
        }

        if (pendingStop) {
            pendingStop = false;
            StopGame();
            RefreshWorlds();
            page = Page::Title;
        }
        if (vsync != appliedVsync) {
            glfwSwapInterval(vsync ? 1 : 0);
            appliedVsync = vsync;
        }
        glfwSwapBuffers(window);
        lap(5);
        if (bench) {
            benchFrames++;
            if (glfwGetTime() - frameStart > benchWorst) {
                benchWorst = glfwGetTime() - frameStart;
                for (int i = 0; i < 7; i++) benchWorstSections[i] = benchFrame[i];
            }
            if (now - benchLast >= 1.0) {
                const double n = benchFrames;
                // (The worst frame, and where its time went, when it stood out.)
                if (benchWorst > 0.025)
                    std::printf("[bench] worst frame %.1f ms: network %.1f  update %.1f  raycast %.1f  render %.1f  gpu %.1f  ui %.1f  swap %.1f\n", benchWorst * 1e3,
                                benchWorstSections[0] * 1e3, benchWorstSections[1] * 1e3, benchWorstSections[2] * 1e3, benchWorstSections[3] * 1e3,
                                benchWorstSections[6] * 1e3, benchWorstSections[4] * 1e3, benchWorstSections[5] * 1e3);
                benchWorst = 0.0;
                std::printf("[bench] %.1f fps | network %.2f  update %.2f  raycast %.2f  render %.2f  gpu %.2f  ui %.2f  swap %.2f ms | %zu chunks, %zu meshed, %zu drawn\n",
                            n / (now - benchLast), benchSections[0] / n * 1e3, benchSections[1] / n * 1e3, benchSections[2] / n * 1e3,
                            benchSections[3] / n * 1e3, benchSections[6] / n * 1e3, benchSections[4] / n * 1e3, benchSections[5] / n * 1e3, world->GetLoadedChunkCount(),
                            world->GetMeshedChunkCount(), world->GetDrawnChunkCount());
                std::fflush(stdout);
                benchLast = now;
                benchFrames = 0;
                for (double& s : benchSections) s = 0.0;
            }
        }
    }
}

void Application::UpdateSky(float deltaTime, FrameParams& params) {
    // The sun goes round once a day, rising in the east (+x) at time 0.
    const float angle = client->GetTimeOfDay() * 6.2831853f;
    const float height = std::sin(angle);
    params.sunDirection = glm::normalize(glm::vec3(std::cos(angle), height, 0.35f));
    // Full daylight once the sun is a little up, dark once it's a little down.
    const float day = glm::smoothstep(-0.12f, 0.25f, height);
    // Warm sky while the sun is near the horizon.
    const float warmth = day * (1.0f - glm::smoothstep(0.0f, 0.4f, std::abs(height)));
    params.skyTint = glm::mix(glm::vec3(1.0f), glm::vec3(1.0f, 0.72f, 0.5f), warmth * 0.7f);

    // Rain dims the day and greys the sky; it doesn't fall where something is overhead
    // (eased, so walking under a roof doesn't cut it off in one frame).
    const float rain = client->GetRain();
    sheltered = false;
    const glm::ivec3 eye(glm::floor(camera.GetPosition()));
    for (int y = eye.y + 1; y <= eye.y + 48 && !sheltered; y++) {
        Block block;
        if (world->GetBlock(eye.x, y, eye.z, block) && !block.IsAir() && !block.IsPlant()) sheltered = true;
    }
    // Snow where it's cold: the ground under the open sky here is snowy (snow, snowy
    // grass, ice or a pine's needles).
    {
        bool snowy = false;
        for (int y = eye.y + 2; y >= eye.y - 24; y--) {
            Block block;
            if (!world->GetBlock(eye.x, y, eye.z, block)) break;
            if (block.IsAir() || block.IsPlant()) continue;
            snowy = block.type == BlockType::SNOW || block.type == BlockType::SNOWY_GRASS || block.type == BlockType::ICE || block.type == BlockType::PINE_LEAVES;
            break;
        }
        params.snow = snowy;
    }
    // The rain falls all round, but not under cover: for each column round the camera,
    // the top of the highest solid block over it (from well above down to under our
    // feet), refreshed a few rows a frame (all of them after a jump).
    {
        constexpr int N = Rain::kCoverSize, R = N / 2;
        if (rainCover.size() != static_cast<size_t>(N) * N) rainCover.assign(static_cast<size_t>(N) * N, -1e6f);
        const bool jumped = glm::any(glm::greaterThan(glm::abs(eye - rainCoverAt), glm::ivec3(3)));
        const int rows = jumped ? N : 6;
        for (int r = 0; r < rows; r++) {
            const int dz = (rainCoverRow + r) % N - R;
            for (int dx = -R; dx <= R; dx++) {
                const int x = eye.x + dx, z = eye.z + dz;
                float top = -1e6f;
                for (int y = eye.y + 48; y >= eye.y - 14; y--) {
                    Block b;
                    if (world->GetBlock(x, y, z, b) && !b.IsAir() && !b.IsPlant()) {
                        top = static_cast<float>(y + 1);
                        break;
                    }
                }
                rainCover[static_cast<size_t>(((z % N) + N) % N) * N + ((x % N) + N) % N] = top;
            }
        }
        rainCoverRow = (rainCoverRow + rows) % N;
        if (jumped) rainCoverAt = eye;
        else if (rainCoverRow < rows) rainCoverAt = eye; // (a whole pass done)
        // How open the sky is just round us: the share of the nearby columns open above
        // our head. Under a roof the rain is muffled, not gone.
        int open = 0, all = 0;
        for (int dz = -3; dz <= 3; dz++)
            for (int dx = -3; dx <= 3; dx++) {
                const int x = eye.x + dx, z = eye.z + dz;
                open += rainCover[static_cast<size_t>(((z % N) + N) % N) * N + ((x % N) + N) % N] <= static_cast<float>(eye.y) ? 1 : 0;
                all++;
            }
        rainOpen += (static_cast<float>(open) / all - rainOpen) * std::min(1.0f, deltaTime * 3.0f);
        params.rainCover = rainCover.data();
    }
    const float target = params.underwater ? 0.0f : rain;
    const float ease = std::min(1.0f, deltaTime * 4.0f);
    rainHere += (target - rainHere) * ease;
    params.rain = rainHere;
    // A rainbow while a daytime shower is light (fading in or out), the sun well up.
    params.rainbow = (rain > 0.05f && rain < 0.6f && !client->IsStorm()) ? std::min(1.0f, (0.6f - rain) * 3.0f) * std::clamp(day * 2.0f - 0.5f, 0.0f, 1.0f) : 0.0f;
    // A thunderstorm is darker still; a lightning flash lights everything up.
    const float storm = client->IsStorm() ? rain : 0.0f;
    params.overcast = std::min(1.0f, rain + 0.3f * storm);
    params.sunBrightness = day * (1.0f - 0.45f * rain - 0.25f * storm);
    params.sunBrightness = std::max(params.sunBrightness, lightningFlash);
}

void Application::UpdateNetwork() {
    client->Poll(0.004); // (at most 4 ms a frame: a burst of chunks is spread over frames)

    // Joined: start at the spawn point the server picked, and drop onto the terrain
    // there once it has arrived.
    if (!joined && client->IsWelcomed()) {
        joined = true;
        player.Teleport(client->GetSpawn());
        player.ResetSpawn();
        // Rejoining a saved world: back where we left, facing the same way.
        if (client->GetSpawnFlags() & net::kWelcomeResume) {
            player.ResumeAt(client->GetSpawn());
            camera.SetLook(client->GetSpawnYaw(), client->GetSpawnPitch());
        }
    }
    // VOXEL_RUN: a second after landing (so the server knows where we are), its lines.
    if (joined && !startCommands.empty() && (player.IsSpawned() || player.IsFlying())) {
        if (landedAt == 0.0) landedAt = glfwGetTime();
        if (glfwGetTime() - landedAt > 1.0) {
            size_t start = 0;
            while (start <= startCommands.size()) {
                const size_t end = std::min(startCommands.find(';', start), startCommands.size());
                if (end > start) client->SendChat(startCommands.substr(start, end - start));
                start = end + 1;
            }
            startCommands.clear();
        }
    }
    const glm::vec3 knockback = client->TakeKnockback();
    if (glm::length(knockback) > 0.0f && !player.IsFlying()) player.Push(knockback);
    if (auto correction = client->TakeCorrection()) {
        player.Teleport(*correction);
        // Corrected into terrain, or somewhere not loaded yet (back to the spawn point,
        // or a bed far off): wait for it to load, then stay there if there's room
        // (climbing out if not).
        if (!player.IsFlying() && Collision::BoxBlocked(*world, player.GetBounds())) player.ResumeAt(*correction);
    }
}

void Application::DrawCrosshair() {
    // Only while aiming: not with the cursor free, the inventory open or typing.
    if (!captureCursor || inventoryOpen || chatOpen || !client->IsWelcomed()) return;
    ui::DrawList* draw = ui::GetForegroundDrawList();
    const ui::Viewport* viewport = ui::GetMainViewport();
    const ui::Vec2 c(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    constexpr float kArm = 9.0f, kGap = 0.0f;
    // A dark outline under a white cross, so it shows on sky and snow alike.
    for (const auto& [color, width] : { std::pair<ui::Color, float>{ UI_COL32(0, 0, 0, 170), 4.0f }, { UI_COL32(255, 255, 255, 230), 2.0f } }) {
        draw->AddLine(ui::Vec2(c.x - kArm, c.y), ui::Vec2(c.x - kGap, c.y), color, width);
        draw->AddLine(ui::Vec2(c.x + kGap, c.y), ui::Vec2(c.x + kArm, c.y), color, width);
        draw->AddLine(ui::Vec2(c.x, c.y - kArm), ui::Vec2(c.x, c.y - kGap), color, width);
        draw->AddLine(ui::Vec2(c.x, c.y + kGap), ui::Vec2(c.x, c.y + kArm), color, width);
    }
    // Winding up a throw: a bar under the crosshair, filling as it's held.
    if (throwHeld && ThrowStrength() > 0.0f) {
        const float strength = ThrowStrength();
        constexpr float kWidth = 44.0f, kHeight = 5.0f, kBelow = 14.0f;
        const ui::Vec2 lo(c.x - kWidth * 0.5f, c.y + kBelow), hi(c.x + kWidth * 0.5f, c.y + kBelow + kHeight);
        draw->AddRectFilled(ui::Vec2(lo.x - 1, lo.y - 1), ui::Vec2(hi.x + 1, hi.y + 1), UI_COL32(0, 0, 0, 170), 2.0f);
        const ui::Color fill = strength >= 1.0f ? UI_COL32(255, 210, 80, 240) : UI_COL32(255, 255, 255, 220);
        draw->AddRectFilled(lo, ui::Vec2(lo.x + kWidth * strength, hi.y), fill, 2.0f);
    }
    // Looking at a signal block: its channels and whether it's on.
    Block target;
    auto label = [&](const char* text) {
        const ui::Vec2 size = ui::CalcTextSize(text);
        const ui::Vec2 p(c.x - size.x * 0.5f, c.y + 22.0f);
        draw->AddRectFilled(ui::Vec2(p.x - 6, p.y - 3), ui::Vec2(p.x + size.x + 6, p.y + size.y + 3), UI_COL32(0, 0, 0, 140), 4.0f);
        draw->AddText(p, UI_COL32(255, 255, 255, 235), text);
    };
    if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
        at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && Signals::KindOf(target.type) != Signals::Kind::None) {
        const auto& infos = client->GetSignals();
        auto it = infos.find(at->block);
        char text[160];
        const net::SignalInfo info = it != infos.end() ? it->second : net::SignalInfo{};
        auto ch = [](int c) { return c ? "ch " + std::to_string(c) : std::string("no channel"); };
        const char* onOff = info.on ? "on" : "off";
        switch (Signals::KindOf(target.type)) {
        case Signals::Kind::Lever: std::snprintf(text, sizeof text, "Lever: %s, %s", target.type == BlockType::LEVER_ON ? "on" : "off", ch(info.out).c_str()); break;
        case Signals::Kind::Plate: std::snprintf(text, sizeof text, "Plate: %s", ch(info.out).c_str()); break;
        case Signals::Kind::Gate: {
            const auto mode = static_cast<Signals::Mode>(info.mode);
            const std::string inputs = Signals::Inputs(mode) == 2 ? std::to_string(info.a) + "+" + std::to_string(info.b) : std::to_string(info.a);
            std::snprintf(text, sizeof text, "%s gate: %s, %s > %s", Signals::ModeName(mode), onOff, inputs.c_str(), ch(info.out).c_str());
            break;
        }
        case Signals::Kind::Lamp: std::snprintf(text, sizeof text, "Lamp: %s", ch(info.a).c_str()); break;
        case Signals::Kind::Valve: std::snprintf(text, sizeof text, "Valve: %s%s", IsOpenValve(target.type) ? "open" : "shut", info.a ? (", " + ch(info.a)).c_str() : ""); break;
        case Signals::Kind::Piston: std::snprintf(text, sizeof text, "Piston: %s", ch(info.a).c_str()); break;
        case Signals::Kind::Launcher: std::snprintf(text, sizeof text, "Launcher: %s, %d/s", ch(info.a).c_str(), static_cast<int>(Power::LaunchSpeed(info.b))); break;
        case Signals::Kind::Sprinkler: std::snprintf(text, sizeof text, "Sprinkler: %s", info.a ? ch(info.a).c_str() : "auto"); break;
        case Signals::Kind::Sluice: std::snprintf(text, sizeof text, "Sluice gate: %s", ch(info.a).c_str()); break;
        default: {
            const auto k = Signals::KindOf(target.type);
            const char* what = k == Signals::Kind::WaterSensor ? "Water sensor" : k == Signals::Kind::PressureSensor ? "Pressure sensor"
                             : k == Signals::Kind::DaylightSensor ? "Daylight sensor" : k == Signals::Kind::MachineSensor ? "Machine sensor"
                             : k == Signals::Kind::HeatSensor ? "Heat sensor" : "Player sensor";
            std::snprintf(text, sizeof text, "%s: %s, %s", what, onOff, ch(info.out).c_str());
        }
        }
        label(text);
        return;
    }
    // Looking at a filter pipe: what it lets through.
    if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
        at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && target.type == BlockType::FILTER_PIPE) {
        const ItemId only = Items::FilterOf(target);
        label(("Filter: " + (only == Items::kNone ? std::string("anything") : ItemName(only))).c_str());
        return;
    }
}

void Application::UpdateMachines(FrameParams& params, const std::function<glm::vec2(const glm::vec3&)>& lightAt) {
    const double now = Client::Now();
    const float dt = static_cast<float>(std::min(0.1, now - lastMachinesTime));
    lastMachinesTime = now;
    std::unordered_map<glm::ivec3, uint8_t, Coords::IVec3Hash> speeds;
    for (const auto& m : client->GetMachines()) {
        if (m.type == static_cast<uint8_t>(BlockType::WATER_WHEEL)) speeds[m.pos] = m.speed;
        // A working drill throws up dust where it's digging, and grinds now and then.
        else if (m.type == static_cast<uint8_t>(BlockType::DRILL) && m.state == static_cast<uint8_t>(Power::DrillState::Working)) {
            float& due = drillDust[m.pos];
            due -= dt;
            if (due <= 0.0f) {
                due = 0.35f;
                const glm::ivec3 at = m.pos - glm::ivec3(0, std::max<int>(1, m.depth), 0);
                Block dug;
                const BlockType shown = world->GetBlock(at.x, at.y - 1, at.z, dug) && !dug.IsAir() ? dug.type : BlockType::STONE;
                SpawnParticles(at, shown, 4, 1.2f);
                if (std::fmod(now, 1.5) < 0.35) PlayAt("dig_hard", glm::vec3(m.pos) + 0.5f, 0.35f);
            }
        }
    }
    if (drillDust.size() > 256) drillDust.clear();
    // Air launchers: a pneumatic thunk and a puff of air out of the front as each fires.
    {
        std::unordered_map<glm::ivec3, bool, Coords::IVec3Hash> firing;
        glm::vec2 s0, s1;
        renderer->GetAtlas().GetTileUV(renderer->GetAtlas().GetIndex("spray"), s0, s1);
        static std::mt19937 puffRng(77);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        for (const auto& m : client->GetMachines()) {
            if (m.type != static_cast<uint8_t>(BlockType::LAUNCHER)) continue;
            const bool on = m.state == static_cast<uint8_t>(Power::DrillState::Working);
            firing[m.pos] = on;
            if (!on || launchersFiring[m.pos]) continue;
            PlayAt("piston", m.pos, 1.0f);
            Block l;
            if (!world->GetBlock(m.pos.x, m.pos.y, m.pos.z, l)) continue;
            const glm::vec3 dir(l.Facing());
            for (int i = 0; i < 10 && particles.size() < 3000; i++) {
                Particle p;
                p.position = glm::vec3(m.pos) + glm::vec3(0.5f) + dir * 0.6f + glm::vec3(unit(puffRng) - 0.5f, unit(puffRng) - 0.5f, unit(puffRng) - 0.5f) * 0.5f;
                p.velocity = dir * (2.0f + 3.0f * unit(puffRng)) + glm::vec3(unit(puffRng) - 0.5f, unit(puffRng) - 0.5f, unit(puffRng) - 0.5f);
                p.maxLife = p.life = 0.5f + 0.4f * unit(puffRng);
                p.size = 0.25f + 0.2f * unit(puffRng);
                p.uv = glm::vec4(s0, s1);
                p.mist = true;
                particles.push_back(p);
            }
        }
        launchersFiring.swap(firing);
        // Sprinklers spraying: drops raining down round them, and a hiss of water.
        const double nowSecs = glfwGetTime();
        const float sprayDt = static_cast<float>(std::min(0.1, nowSecs - lastSprinklerSpray));
        lastSprinklerSpray = nowSecs;
        bool hiss = false;
        for (const auto& m : client->GetMachines()) {
            if (m.type != static_cast<uint8_t>(BlockType::SPRINKLER) || m.state != static_cast<uint8_t>(Power::DrillState::Working)) continue;
            if (glm::distance(glm::vec3(m.pos), camera.GetPosition()) > 48.0f) continue;
            float rate = sprayDt * 120.0f;
            while (rate > 0.0f && particles.size() < 3000) {
                if (rate < 1.0f && unit(puffRng) >= rate) break;
                rate -= 1.0f;
                const float angle = unit(puffRng) * 6.2831853f, out = 1.5f + 3.5f * unit(puffRng);
                Particle p;
                p.position = glm::vec3(m.pos) + glm::vec3(0.5f, -0.05f, 0.5f);
                p.velocity = glm::vec3(std::cos(angle) * out, -0.5f - unit(puffRng), std::sin(angle) * out);
                p.maxLife = p.life = 0.8f + 0.6f * unit(puffRng);
                p.size = 0.06f + 0.05f * unit(puffRng);
                p.uv = glm::vec4(s0, s1);
                particles.push_back(p);
            }
            if (!hiss && nowSecs >= nextSprinklerHiss) {
                PlayAt("splash", m.pos, 0.35f);
                hiss = true;
            }
        }
        if (hiss) nextSprinklerHiss = nowSecs + 0.5;
    }

    // Pressure gauges, their needles easing towards the pressure the server last sent.
    std::unordered_map<glm::ivec3, uint8_t, Coords::IVec3Hash> readings;
    for (const auto& m : client->GetMachines())
        if (m.type == static_cast<uint8_t>(BlockType::PRESSURE_GAUGE)) readings[m.pos] = m.pressure;
    std::unordered_map<glm::ivec3, float, Coords::IVec3Hash> needles;
    for (const auto& pos : world->GetModelBlocks()) {
        Block g;
        if (!world->GetBlock(pos.x, pos.y, pos.z, g) || g.type != BlockType::PRESSURE_GAUGE) continue;
        float shown = gaugeNeedles.count(pos) ? gaugeNeedles[pos] : 0.0f;
        const float reading = readings.count(pos) ? readings[pos] / 255.0f : 0.0f;
        shown += (reading - shown) * std::min(1.0f, dt * 4.0f);
        needles[pos] = shown;
        params.gauges.push_back({ glm::vec3(pos) + 0.5f, g.Facing(), shown, lightAt(glm::vec3(pos)) });
    }
    gaugeNeedles.swap(needles);

    // Doors swing open and shut over a quarter of a second (eased in and out) rather than
    // jumping. One seen for the first time is drawn as it is.
    glm::vec2 door0, door1;
    renderer->GetAtlas().GetTileUV(renderer->GetAtlas().GetIndex("door"), door0, door1);
    std::unordered_map<glm::ivec3, float, Coords::IVec3Hash> swings;
    for (const auto& pos : world->GetModelBlocks()) {
        Block d;
        if (!world->GetBlock(pos.x, pos.y, pos.z, d) || (d.type != BlockType::DOOR && d.type != BlockType::DOOR_OPEN)) continue;
        const float target = d.type == BlockType::DOOR_OPEN ? 1.0f : 0.0f;
        float swing = doorSwings.count(pos) ? doorSwings[pos] : target;
        swing = target > swing ? std::min(target, swing + dt * 4.0f) : std::max(target, swing - dt * 4.0f);
        swings[pos] = swing;
        const float eased = swing * swing * (3.0f - 2.0f * swing);
        // Its light: the face we see is lit by the cell on our side of it (shut: the
        // doorway's inside or out), else (that side walled up, or the door swinging) by
        // its own cell, which light passes into. (Not by the general look-up, which takes
        // a door's cell, a full block, for the inside of a wall and so finds no light at
        // all, drawing it in full daylight even in a dark room.)
        auto cellLight = [&](const glm::ivec3& c, glm::vec2& out) {
            int blockLight = 0, skyLight = 0;
            Block b;
            if (!world->GetLight(c.x, c.y, c.z, blockLight, skyLight)) return false;
            if (world->GetBlock(c.x, c.y, c.z, b) && b.BlocksLight()) return false;
            out = glm::vec2(skyLight, blockLight);
            return true;
        };
        const glm::ivec3 facing = d.Facing();
        const glm::vec3 centre = glm::vec3(pos) + glm::vec3(0.5f);
        const glm::ivec3 ourSide = glm::dot(camera.GetPosition() - centre, glm::vec3(facing)) >= 0.0f ? pos + facing : pos - facing;
        glm::vec2 light(0.0f);
        if (!(eased < 0.05f && cellLight(ourSide, light)) && !cellLight(pos, light)) light = lightAt(centre);
        for (const auto& h : params.handLights) light.y = std::max(light.y, h.w - glm::distance(centre, glm::vec3(h)));
        params.doors.push_back({ glm::vec3(pos), facing, eased, glm::vec4(door0, door1), light, d.DoorMirrored() });
    }
    doorSwings.swap(swings);

    // Levers: the handle tips over in a tenth of a second when flipped.
    std::unordered_map<glm::ivec3, float, Coords::IVec3Hash> tips;
    for (const auto& pos : world->GetModelBlocks()) {
        Block l;
        if (!world->GetBlock(pos.x, pos.y, pos.z, l) || !IsLeverType(l.type)) continue;
        const float target = l.type == BlockType::LEVER_ON ? 1.0f : 0.0f;
        float on = leverSwings.count(pos) ? leverSwings[pos] : target;
        on = target > on ? std::min(target, on + dt * 10.0f) : std::max(target, on - dt * 10.0f);
        tips[pos] = on;
        params.levers.push_back({ glm::vec3(pos) + glm::vec3(0.5f), l.LeverFacing(), on, lightAt(glm::vec3(pos) + glm::vec3(0.5f)) });
    }
    leverSwings.swap(tips);

    // Formed structures: their models, at their pattern's corner, working or not.
    std::unordered_map<glm::ivec3, uint8_t, Coords::IVec3Hash> states;
    for (const auto& m : client->GetMachines()) states[m.pos] = m.state;
    for (const auto& pos : world->GetModelBlocks()) {
        Block core;
        if (!world->GetBlock(pos.x, pos.y, pos.z, core) || core.type != BlockType::MULTIBLOCK) continue;
        const auto& pattern = Multiblocks::PatternOf(Multiblocks::KindOf(core));
        const bool working = states.count(pos) && states[pos] == static_cast<uint8_t>(Power::DrillState::Working);
        params.structures.push_back({ pattern.id, glm::vec3(Multiblocks::OriginOf(pos, core)), Multiblocks::TurnOf(core), working, lightAt(glm::vec3(pos) + glm::vec3(0.0f, 1.0f, 0.0f)) });
    }

    // Wheels and shafts in lines along their axles turn together, as fast as the
    // fastest wheel on the line (still if the server said nothing). Each line keeps its
    // angle under the position of its first block.
    const std::vector<glm::ivec3> models = world->GetModelBlocks();
    std::unordered_map<glm::ivec3, Block, Coords::IVec3Hash> blocks;
    for (const auto& pos : models) {
        Block b;
        if (world->GetBlock(pos.x, pos.y, pos.z, b) && (b.type == BlockType::WATER_WHEEL || b.type == BlockType::SHAFT)) blocks[pos] = b;
    }
    auto sameLine = [&](const glm::ivec3& p, const glm::ivec3& axle) {
        auto it = blocks.find(p);
        return it != blocks.end() && Power::AxleOf(it->second) == axle;
    };
    std::unordered_map<glm::ivec3, float, Coords::IVec3Hash> angles;
    for (const auto& [pos, block] : blocks) {
        const glm::ivec3 axle = Power::AxleOf(block);
        if (sameLine(pos - axle, axle)) continue; // (not the first of its line)
        float speed = 0.0f;
        std::vector<glm::ivec3> line;
        for (glm::ivec3 p = pos; sameLine(p, axle) && line.size() < 256; p += axle) {
            line.push_back(p);
            if (auto s = speeds.find(p); s != speeds.end()) speed = std::max(speed, s->second / 255.0f);
        }
        float angle = wheelAngles.count(pos) ? wheelAngles[pos] : 0.0f;
        angle = std::fmod(angle + speed * dt * 4.0f, 6.2831853f); // up to two thirds of a turn a second
        angles[pos] = angle;
        for (const auto& p : line) {
            const glm::vec3 centre = glm::vec3(p) + 0.5f;
            params.wheels.push_back({ centre, axle.x != 0, angle, lightAt(glm::vec3(p)), blocks[p].type == BlockType::SHAFT });
        }
    }
    wheelAngles.swap(angles);

    // Machines at work hum where they are: the nearest few within earshot (Audio::SetHums).
    if (!audio) return;
    const glm::vec3 ear = camera.GetPosition();
    struct Near { Audio::Hum hum; float distance; };
    std::vector<Near> near;
    const auto working = static_cast<uint8_t>(Power::DrillState::Working);
    for (const auto& m : client->GetMachines()) {
        const glm::vec3 at = glm::vec3(m.pos) + 0.5f;
        const float distance = glm::distance(at, ear);
        if (distance > 28.0f) continue;
        const char* name = nullptr;
        float volume = 0.5f;
        switch (static_cast<BlockType>(m.type)) {
        case BlockType::WATER_WHEEL:
            if (m.speed > 8) name = "hum_wheel", volume = 0.2f + 0.45f * m.speed / 255.0f; // louder the faster it turns
            break;
        case BlockType::COMPRESSOR: if (m.state == working) name = "hum_compressor", volume = 0.45f; break;
        case BlockType::DRILL: if (m.state == working) name = "hum_drill", volume = 0.55f; break;
        case BlockType::PUMP: if (m.state == working) name = "hum_pump", volume = 0.45f; break;
        case BlockType::ENGINE: if (m.state == working) name = "hum_engine", volume = 0.55f; break;
        case BlockType::TURBINE: if (m.state == working) name = "hum_turbine", volume = 0.5f; break;
        case BlockType::SMELTER:
        case BlockType::SMELTER_LIT: if (m.state == working) name = "hum_furnace", volume = 0.45f; break;
        case BlockType::CRUSHER: if (m.state == working) name = "hum_crusher", volume = 0.55f; break;
        case BlockType::BOILER: if (m.state == working) name = "hum_boiler", volume = 0.45f; break;
        case BlockType::AIR_DIFFUSER: if (m.state == working) name = "hum_diffuser", volume = 0.25f; break;
        case BlockType::HARVESTER:
        case BlockType::EXTRACTOR:
        case BlockType::CRAFTER: if (m.state == working) name = "hum_machine", volume = 0.35f; break;
        case BlockType::MULTIBLOCK: {
            Block core;
            if (m.state != working || !world->GetBlock(m.pos.x, m.pos.y, m.pos.z, core) || core.type != BlockType::MULTIBLOCK) break;
            switch (Multiblocks::KindOf(core)) {
            case Multiblocks::Kind::OilDerrick: name = "hum_derrick", volume = 0.7f; break;
            case Multiblocks::Kind::Refinery: name = "hum_refinery", volume = 0.6f; break;
            case Multiblocks::Kind::BlastFurnace: name = "hum_furnace", volume = 0.75f; break;
            case Multiblocks::Kind::DieselCompressor: name = "hum_engine", volume = 0.75f; break;
            case Multiblocks::Kind::CokeOven: name = "hum_furnace", volume = 0.55f; break;
            default: break;
            }
            break;
        }
        default: break;
        }
        if (name) near.push_back({ { name, at, volume }, distance });
    }
    std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.distance < b.distance; });
    std::vector<Audio::Hum> hums;
    for (size_t i = 0; i < near.size() && i < Audio::kMaxHums; i++) hums.push_back(near[i].hum);
    audio->SetHums(hums);
}

namespace {
// The colour a block shows from above on the minimap (0xAABBGGRR).
uint32_t DefaultMapColour(BlockType type);
uint32_t MapColour(BlockType type) {
    const auto set = BlockData::MapColour(type);
    return set ? *set : DefaultMapColour(type);
}
uint32_t DefaultMapColour(BlockType type) {
    switch (type) {
    case BlockType::GRASS: return 0xFF3E9A4Au;
    case BlockType::LUSH_GRASS: return 0xFF2F8E36u;
    case BlockType::DRY_GRASS: return 0xFF4FA7A6u;
    case BlockType::SNOWY_GRASS:
    case BlockType::SNOW: return 0xFFF0F0F0u;
    case BlockType::DIRT: return 0xFF3C5A86u;
    case BlockType::SAND: return 0xFF8CCDE0u;
    case BlockType::STONE:
    case BlockType::COAL_ORE:
    case BlockType::IRON_ORE:
    case BlockType::PEBBLES: return 0xFF808080u;
    case BlockType::WATER: return 0xFFC07A3Cu;
    case BlockType::LAVA: return 0xFF1A60F0u;
    case BlockType::OBSIDIAN: return 0xFF28101Cu;
    case BlockType::ICE: return 0xFFF0D8B0u;
    case BlockType::LEAVES: return 0xFF2A6E2Au;
    case BlockType::PINE_LEAVES: return 0xFF2E5A2Eu;
    case BlockType::LOG:
    case BlockType::PLANKS:
    case BlockType::CHEST:
    case BlockType::DOOR:
    case BlockType::LADDER: return 0xFF2E5C8Au;
    case BlockType::CACTUS: return 0xFF2C8C3Cu;
    case BlockType::STONE_BRICK:
    case BlockType::MOSSY_BRICK:
    case BlockType::CRUMBLING_BRICK: return 0xFF6A6A6Au;
    case BlockType::LAMP: return 0xFF7AE0F8u;
    case BlockType::GLASS: return 0xFFE0E0D0u;
    case BlockType::BED: return 0xFF3030C0u;
    case BlockType::HAY_BALE: return 0xFF40B8D8u;
    case BlockType::ASPHALT: return 0xFF343030u;
    case BlockType::LIMESTONE: return 0xFF98BCC8u;
    case BlockType::CONCRETE: return 0xFFAAA8A8u;
    case BlockType::GRAVEL: return 0xFF747A80u;
    case BlockType::REINFORCED_CONCRETE: return 0xFF888C90u;
    case BlockType::DEEPSTONE: return 0xFF4C3E3Au;
    case BlockType::GRAVE: return 0xFF8C9490u;
    case BlockType::HEAT_SENSOR: return 0xFF2040A0u;
    case BlockType::SPRINKLER: return 0xFF4090B0u;
    case BlockType::CROP:
    case BlockType::RIPE_WHEAT: return 0xFF46B4C8u;
    default: return 0xFF707070u;
    }
}
} // namespace

namespace {
int64_t MapTileKey(int x, int z) {
    const int tx = x >= 0 ? x / 16 : -((-x + 15) / 16), tz = z >= 0 ? z / 16 : -((-z + 15) / 16);
    return (static_cast<int64_t>(tx) << 32) ^ static_cast<uint32_t>(tz);
}
int MapTileIndex(int x, int z) { return ((x % 16 + 16) % 16) * 16 + ((z % 16 + 16) % 16); }
} // namespace

void Application::LoadMapCache() {
    mapCache.clear();
    mapCacheSavedAt = glfwGetTime();
    std::ifstream in(mapCacheFile, std::ios::binary);
    uint32_t count = 0;
    if (!in.read(reinterpret_cast<char*>(&count), sizeof count) || count > (1u << 22)) return;
    for (uint32_t i = 0; i < count; i++) {
        int64_t key;
        std::array<uint32_t, 256> tile;
        if (!in.read(reinterpret_cast<char*>(&key), sizeof key) || !in.read(reinterpret_cast<char*>(tile.data()), sizeof(uint32_t) * tile.size())) break;
        mapCache[key] = tile;
    }
    mapCacheChanged = false;
}

void Application::SaveMapCache(bool background) {
    if (mapCacheFile.empty() || !mapCacheChanged) return;
    // The bytes, gathered here (quick); written out here or on a thread of their own.
    auto bytes = std::make_shared<std::vector<char>>();
    const uint32_t count = static_cast<uint32_t>(mapCache.size());
    bytes->reserve(sizeof count + count * (sizeof(int64_t) + 256 * sizeof(uint32_t)));
    auto put = [&](const void* p, size_t n) { bytes->insert(bytes->end(), static_cast<const char*>(p), static_cast<const char*>(p) + n); };
    put(&count, sizeof count);
    for (const auto& [key, tile] : mapCache) {
        put(&key, sizeof key);
        put(tile.data(), sizeof(uint32_t) * tile.size());
    }
    mapCacheChanged = false;
    auto write = [file = mapCacheFile, bytes] {
        static std::mutex writing; // (one write at a time)
        std::lock_guard<std::mutex> lock(writing);
        std::error_code ec;
        const std::filesystem::path path(file);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
    };
    if (background) std::thread(write).detach();
    else write();
}

// The whole picture from the cache at once (dark where nothing's been seen), before
// it's redrawn row by row.
void Application::FillMapFromCache() {
    for (int row = 0; row < kMapSize; row++)
        for (int col = 0; col < kMapSize; col++) {
            const int x = mapCentre.x - kMapSize / 2 + col, z = mapCentre.y - kMapSize / 2 + row;
            const auto it = mapCache.find(MapTileKey(x, z));
            const uint32_t cached = it != mapCache.end() ? it->second[MapTileIndex(x, z)] : 0u;
            mapPixels[row * kMapSize + col] = cached ? cached : 0xFF202020u;
        }
    glBindTexture(GL_TEXTURE_2D, mapTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kMapSize, kMapSize, GL_RGBA, GL_UNSIGNED_BYTE, mapPixels.data());
}

void Application::DrawMinimap() {
    if (!client->IsWelcomed()) return;
    const glm::vec3 feet = player.GetFeetPosition();
    const glm::ivec2 here(static_cast<int>(std::floor(feet.x)), static_cast<int>(std::floor(feet.z)));
    if (!mapTexture) {
        glGenTextures(1, &mapTexture);
        glBindTexture(GL_TEXTURE_2D, mapTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        mapPixels.assign(kMapSize * kMapSize, 0xFF202020u);
        mapHeights.assign(kMapSize * kMapSize, INT_MIN);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kMapSize, kMapSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, mapPixels.data());
        mapCentre = here;
        mapRow = 0;
        mapFreshRows = 0;
        FillMapFromCache();
    }
    // Moved far from the picture's centre: start again around here.
    if (std::abs(here.x - mapCentre.x) > kMapSize / 6 || std::abs(here.y - mapCentre.y) > kMapSize / 6) {
        mapCentre = here;
        mapRow = 0;
        mapFreshRows = 0;
        std::fill(mapHeights.begin(), mapHeights.end(), INT_MIN);
        FillMapFromCache();
    }
    // Redrawn at a pace: the whole picture every few seconds (quicker just after it's
    // been centred somewhere new), a row at a time. Each row: the topmost block of
    // each column, from a little above the player down (so caves and overhangs below
    // show the ground over them), shaded by whether the ground rises or falls toward
    // the north (the row before's heights, when it has them).
    const double now = glfwGetTime();
    const float dt = mapLastTime > 0.0 ? static_cast<float>(std::min(0.25, now - mapLastTime)) : 0.0f;
    mapLastTime = now;
    const float seconds = mapFreshRows < kMapSize ? 0.75f : 3.0f;
    // (At most a few rows a frame: a slow frame doesn't make the next one slower catching up.)
    mapRowsDue = std::min(mapRowsDue + dt * kMapSize / seconds, 6.0f);
    const int top = static_cast<int>(feet.y) + 24, bottom = static_cast<int>(feet.y) - 40;
    auto ground = [](const Block& b) { return !b.IsAir() && (!b.IsPlant() || b.type == BlockType::CROP || b.type == BlockType::RIPE_WHEAT); };
    auto solid = [](const Block& b) { return !b.IsAir() && !b.IsPlant(); };
    for (; mapRowsDue >= 1.0f; mapRowsDue -= 1.0f, mapRow = (mapRow + 1) % kMapSize, mapFreshRows++) {
        const int z = mapCentre.y - kMapSize / 2 + mapRow;
        const int northRow = (mapRow + kMapSize - 1) % kMapSize;
        for (int col = 0; col < kMapSize; col++) {
            const int x = mapCentre.x - kMapSize / 2 + col;
            Block b;
            const int found = world->TopSolid(x, z, top, bottom, ground, b);
            const uint32_t colour = found >= bottom ? MapColour(b.type) : 0xFF202020u;
            const int height = found >= bottom ? found : bottom;
            mapHeights[mapRow * kMapSize + col] = height;
            // Relief: a column higher than the one north of it is lit, lower is shaded.
            int northHeight = mapRow > 0 ? mapHeights[northRow * kMapSize + col] : INT_MIN;
            if (northHeight == INT_MIN) {
                Block north;
                const int n = world->TopSolid(x, z - 1, top, bottom, solid, north);
                northHeight = n >= bottom ? n : height;
            }
            const float shade = std::clamp(1.0f + 0.12f * (height - northHeight), 0.65f, 1.3f);
            auto channel = [&](int shift) { return static_cast<uint32_t>(std::min(255.0f, ((colour >> shift) & 0xFF) * shade)) << shift; };
            const uint32_t pixel = 0xFF000000u | channel(0) | channel(8) | channel(16);
            // (Not seen yet: the cached colour stays, and nothing's cached.)
            if (found < bottom && colour == 0xFF202020u) {
                const auto it = mapCache.find(MapTileKey(x, z));
                const uint32_t cached = it != mapCache.end() ? it->second[MapTileIndex(x, z)] : 0u;
                mapPixels[mapRow * kMapSize + col] = cached ? cached : pixel;
                continue;
            }
            mapPixels[mapRow * kMapSize + col] = pixel;
            auto& tile = mapCache[MapTileKey(x, z)];
            if (tile[MapTileIndex(x, z)] != pixel) tile[MapTileIndex(x, z)] = pixel, mapCacheChanged = true;
        }
        glBindTexture(GL_TEXTURE_2D, mapTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, mapRow, kMapSize, 1, GL_RGBA, GL_UNSIGNED_BYTE, &mapPixels[mapRow * kMapSize]);
    }
    // (Saved now and then too, not only at the end of the game.)
    if (mapCacheChanged && now - mapCacheSavedAt > 30.0) {
        mapCacheSavedAt = now;
        SaveMapCache(true);
    }
    // Shown in the top-right corner, north up, with an arrow for the player.
    ui::DrawList* draw = ui::GetForegroundDrawList();
    const ui::Viewport* viewport = ui::GetMainViewport();
    constexpr float kShown = 170.0f;
    const ui::Vec2 lo(viewport->WorkPos.x + viewport->WorkSize.x - kShown - 12.0f, viewport->WorkPos.y + 12.0f), hi(lo.x + kShown, lo.y + kShown);
    draw->AddRectFilled(ui::Vec2(lo.x - 3.0f, lo.y - 3.0f), ui::Vec2(hi.x + 3.0f, hi.y + 3.0f), UI_COL32(0, 0, 0, 150), 4.0f);
    draw->AddImage(static_cast<ui::TextureId>(static_cast<uintptr_t>(mapTexture)), lo, hi);
    const float scale = kShown / kMapSize;
    const ui::Vec2 me(lo.x + (feet.x - (mapCentre.x - kMapSize / 2)) * scale, lo.y + (feet.z - (mapCentre.y - kMapSize / 2)) * scale);
    // Dwarf sites: a camp a red triangle, a mine a brown square, a market a green dot, a
    // stronghold a purple diamond (grey once its king has fallen). Those off the map
    // sit at its edge, smaller, the way to them.
    for (const auto& site : client->sites) {
        ui::Vec2 at(lo.x + (site.centre.x + 0.5f - (mapCentre.x - kMapSize / 2)) * scale, lo.y + (site.centre.z + 0.5f - (mapCentre.y - kMapSize / 2)) * scale);
        const bool inside = at.x > lo.x + 5 && at.x < hi.x - 5 && at.y > lo.y + 5 && at.y < hi.y - 5;
        if (!inside) {
            // (Along the line from the middle to it, stopped at the edge.)
            const ui::Vec2 c((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
            const float dx = at.x - c.x, dy = at.y - c.y, half = kShown * 0.5f - 6.0f;
            const float k = half / std::max(std::abs(dx), std::abs(dy));
            at = ui::Vec2(c.x + dx * k, c.y + dy * k);
        }
        const float s = inside ? 5.0f : 3.0f;
        const auto kind = static_cast<Dwarves::SiteKind>(site.kind);
        const ui::Color dark = UI_COL32(0, 0, 0, 200);
        switch (kind) {
        case Dwarves::SiteKind::Camp:
            draw->AddTriangleFilled(ui::Vec2(at.x, at.y - s - 1), ui::Vec2(at.x - s - 1, at.y + s), ui::Vec2(at.x + s + 1, at.y + s), dark);
            draw->AddTriangleFilled(ui::Vec2(at.x, at.y - s), ui::Vec2(at.x - s, at.y + s - 1), ui::Vec2(at.x + s, at.y + s - 1), UI_COL32(230, 70, 50, 255));
            break;
        case Dwarves::SiteKind::Mine:
            draw->AddRectFilled(ui::Vec2(at.x - s - 1, at.y - s - 1), ui::Vec2(at.x + s + 1, at.y + s + 1), dark);
            draw->AddRectFilled(ui::Vec2(at.x - s, at.y - s), ui::Vec2(at.x + s, at.y + s), site.fallen ? UI_COL32(120, 110, 100, 255) : UI_COL32(170, 110, 60, 255));
            break;
        case Dwarves::SiteKind::Market:
            draw->AddCircleFilled(at, s + 1, dark);
            draw->AddCircleFilled(at, s, UI_COL32(80, 210, 90, 255));
            break;
        default:
            draw->AddQuadFilled(ui::Vec2(at.x, at.y - s - 2), ui::Vec2(at.x + s + 2, at.y), ui::Vec2(at.x, at.y + s + 2), ui::Vec2(at.x - s - 2, at.y), dark);
            draw->AddQuadFilled(ui::Vec2(at.x, at.y - s - 1), ui::Vec2(at.x + s + 1, at.y), ui::Vec2(at.x, at.y + s + 1), ui::Vec2(at.x - s - 1, at.y),
                                site.fallen ? UI_COL32(130, 130, 130, 255) : UI_COL32(170, 80, 220, 255));
        }
    }
    // Our bed: a little house (at the edge, if it's off the map).
    if (client->bed) {
        ui::Vec2 at(lo.x + (client->bed->x + 0.5f - (mapCentre.x - kMapSize / 2)) * scale, lo.y + (client->bed->z + 0.5f - (mapCentre.y - kMapSize / 2)) * scale);
        const ui::Vec2 c((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        const float dx = at.x - c.x, dy = at.y - c.y, half = kShown * 0.5f - 6.0f;
        if (std::max(std::abs(dx), std::abs(dy)) > half) {
            const float k = half / std::max(std::abs(dx), std::abs(dy));
            at = ui::Vec2(c.x + dx * k, c.y + dy * k);
        }
        draw->AddRectFilled(ui::Vec2(at.x - 5, at.y - 1), ui::Vec2(at.x + 5, at.y + 6), UI_COL32(0, 0, 0, 220));
        draw->AddTriangleFilled(ui::Vec2(at.x, at.y - 7), ui::Vec2(at.x - 7, at.y), ui::Vec2(at.x + 7, at.y), UI_COL32(0, 0, 0, 220));
        draw->AddRectFilled(ui::Vec2(at.x - 4, at.y), ui::Vec2(at.x + 4, at.y + 5), UI_COL32(240, 240, 240, 255));
        draw->AddTriangleFilled(ui::Vec2(at.x, at.y - 6), ui::Vec2(at.x - 6, at.y), ui::Vec2(at.x + 6, at.y), UI_COL32(220, 60, 50, 255));
    }
    // Where we last died: a red cross (at the edge, if it's off the map).
    if (deathSpot) {
        ui::Vec2 at(lo.x + (deathSpot->x - (mapCentre.x - kMapSize / 2)) * scale, lo.y + (deathSpot->z - (mapCentre.y - kMapSize / 2)) * scale);
        const ui::Vec2 c((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        const float dx = at.x - c.x, dy = at.y - c.y, half = kShown * 0.5f - 6.0f;
        if (std::max(std::abs(dx), std::abs(dy)) > half) {
            const float k = half / std::max(std::abs(dx), std::abs(dy));
            at = ui::Vec2(c.x + dx * k, c.y + dy * k);
        }
        for (const float w : { 4.0f, 2.2f }) {
            const ui::Color col = w > 3.0f ? UI_COL32(0, 0, 0, 220) : UI_COL32(255, 60, 50, 255);
            draw->AddLine(ui::Vec2(at.x - 5, at.y - 5), ui::Vec2(at.x + 5, at.y + 5), col, w);
            draw->AddLine(ui::Vec2(at.x - 5, at.y + 5), ui::Vec2(at.x + 5, at.y - 5), col, w);
        }
    }
    // Other players: dots.
    for (const auto& other : client->GetRemotePlayers()) {
        const ui::Vec2 at(lo.x + (other.feet.x - (mapCentre.x - kMapSize / 2)) * scale, lo.y + (other.feet.z - (mapCentre.y - kMapSize / 2)) * scale);
        if (at.x > lo.x && at.x < hi.x && at.y > lo.y && at.y < hi.y) draw->AddCircleFilled(at, 3.5f, UI_COL32(255, 220, 80, 255));
    }
    const float yaw = glm::radians(camera.GetYaw());
    const ui::Vec2 f(std::cos(yaw), std::sin(yaw)), r(-f.y, f.x);
    draw->AddTriangleFilled(ui::Vec2(me.x + f.x * 7.0f, me.y + f.y * 7.0f), ui::Vec2(me.x - f.x * 4.0f + r.x * 4.5f, me.y - f.y * 4.0f + r.y * 4.5f),
                            ui::Vec2(me.x - f.x * 4.0f - r.x * 4.5f, me.y - f.y * 4.0f - r.y * 4.5f), UI_COL32(255, 255, 255, 255));
    draw->AddText(ui::Vec2(lo.x + kShown * 0.5f - 4.0f, lo.y + 2.0f), UI_COL32(255, 110, 100, 255), "N");
}

void Application::DrawPlayerList() {
    // Everyone here: you first, then the others with how far off they are and which way.
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + 12.0f), ui::Cond_Always);
    ui::SetNextWindowBgAlpha(0.6f);
    if (ui::Begin("Players", nullptr, ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoInputs | ui::WindowFlags_NoSavedSettings |
                                             ui::WindowFlags_NoTitleBar | ui::WindowFlags_NoFocusOnAppearing)) {
        const auto others = client->GetRemotePlayers();
        ui::Text("Players online: %d", static_cast<int>(others.size()) + 1);
        ui::Separator();
        ui::TextColored(ui::Vec4(1.0f, 0.9f, 0.5f, 1.0f), "%s (you)", client->GetMyName().c_str());
        const glm::vec3 me = player.GetFeetPosition();
        for (const auto& o : others) {
            const glm::vec3 d = o.feet - me;
            const float bearing = std::fmod(glm::degrees(std::atan2(d.z, d.x)) + 90.0f + 720.0f, 360.0f);
            static const char* kWays[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
            ui::Text("%s   %d blocks %s", o.name.empty() ? "?" : o.name.c_str(), static_cast<int>(glm::length(d)),
                        kWays[static_cast<int>((bearing + 22.5f) / 45.0f) % 8]);
        }
    }
    ui::End();
}

void Application::DrawDeath() {
    // A new death: where it was (where we were half a second before the news: the
    // respawn may have come first).
    if (client->GetLastDeath() > 0.0 && client->GetLastDeath() != deathSeen) {
        deathSeen = client->GetLastDeath();
        deathSpot.reset();
        for (const auto& [when, feet] : feetHistory)
            if (when <= deathSeen - 0.5) deathSpot = feet;
        if (!deathSpot && !feetHistory.empty()) deathSpot = feetHistory.front().second;
    }
    if (deathSpot && glm::distance(glm::vec2(deathSpot->x, deathSpot->z), glm::vec2(player.GetFeetPosition().x, player.GetFeetPosition().z)) < 3.0f &&
        Client::Now() - deathSeen > 5.0)
        deathSpot.reset(); // (back there: found)
    feetHistory.emplace_back(Client::Now(), player.GetFeetPosition());
    while (feetHistory.size() > 2 && feetHistory.front().first < Client::Now() - 3.0) feetHistory.pop_front();
    const double since = Client::Now() - client->GetLastDeath();
    if (since > 4.0 || since < 0.0) return;
    // How it happened: the latest line in chat about us dying ("Name was ...").
    std::string how;
    const std::string me = client->GetMyName() + " ";
    for (const auto& line : client->GetChat())
        if (line.text.rfind(me, 0) == 0) {
            const std::string rest = line.text.substr(me.size());
            for (const char* death : { "was ", "drowned", "suffocated", "ran out of breath", "starved", "fell ", "walked into" })
                if (rest.rfind(death, 0) == 0) how = "You " + (rest.rfind("was ", 0) == 0 ? "were " + rest.substr(4) : rest);
        }
    const float fade = static_cast<float>(std::min(1.0, std::min(since * 6.0, (4.0 - since) * 1.5)));
    ui::DrawList* draw = ui::GetForegroundDrawList();
    const ui::Viewport* viewport = ui::GetMainViewport();
    const ui::Vec2 lo = viewport->WorkPos, hi(lo.x + viewport->WorkSize.x, lo.y + viewport->WorkSize.y);
    draw->AddRectFilledMultiColor(lo, hi, UI_COL32(90, 0, 0, static_cast<int>(150 * fade)), UI_COL32(90, 0, 0, static_cast<int>(150 * fade)),
                                  UI_COL32(20, 0, 0, static_cast<int>(200 * fade)), UI_COL32(20, 0, 0, static_cast<int>(200 * fade)));
    const char* title = "You died!";
    const float cx = lo.x + viewport->WorkSize.x * 0.5f, cy = lo.y + viewport->WorkSize.y * 0.38f;
    const ui::Font* font = ui::GetFont();
    const float big = ui::GetFontSize() * 2.6f;
    const ui::Vec2 titleSize = font->Measure(big, title);
    draw->AddText(font, big, ui::Vec2(cx - titleSize.x * 0.5f + 3.0f, cy + 3.0f), UI_COL32(0, 0, 0, static_cast<int>(200 * fade)), title);
    draw->AddText(font, big, ui::Vec2(cx - titleSize.x * 0.5f, cy), UI_COL32(255, 90, 80, static_cast<int>(255 * fade)), title);
    if (!how.empty()) {
        const ui::Vec2 size = ui::CalcTextSize(how.c_str());
        draw->AddText(ui::Vec2(cx - size.x * 0.5f, cy + titleSize.y + 14.0f), UI_COL32(255, 230, 220, static_cast<int>(240 * fade)), how.c_str());
    }
}

void Application::DrawBossBar() {
    float health = -1.0f, best = 32.0f;
    const char* title = "The Dwarf King";
    for (const auto& c : client->GetCreatures())
        if (c.type == CreatureType::DwarfKing || c.type == CreatureType::ForgeGolem) {
            const float d = glm::distance(c.feet, camera.GetPosition());
            if (d < best) best = d, health = std::clamp(c.health, 0.0f, 1.0f), title = c.type == CreatureType::DwarfKing ? "The Dwarf King" : "Forge golem";
        }
    if (health < 0.0f) return;
    ui::DrawList* draw = ui::GetForegroundDrawList();
    const ui::Viewport* viewport = ui::GetMainViewport();
    const float width = std::min(520.0f, viewport->WorkSize.x * 0.5f), height = 14.0f;
    const ui::Vec2 corner(viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5f, viewport->WorkPos.y + 118.0f);
    const ui::Vec2 titleSize = ui::CalcTextSize(title);
    draw->AddText(ui::Vec2(corner.x + (width - titleSize.x) * 0.5f, corner.y - titleSize.y - 4.0f), UI_COL32(255, 220, 140, 255), title);
    draw->AddRectFilled(ui::Vec2(corner.x - 2, corner.y - 2), ui::Vec2(corner.x + width + 2, corner.y + height + 2), UI_COL32(20, 12, 8, 210), 3.0f);
    draw->AddRectFilled(corner, ui::Vec2(corner.x + width * health, corner.y + height), health < 0.5f ? UI_COL32(220, 70, 40, 255) : UI_COL32(200, 150, 40, 255), 2.0f);
}

void Application::DrawToast() {
    // Achievements arrive as chat lines starting with the toast mark: the newest shows
    // as a banner under the compass for four seconds, sliding in and fading out.
    const auto& chat = client->GetChat();
    const std::string mark = Achievements::kToastMark;
    // (Counted by lines ever received: the log keeps only the last hundred, so an index
    // into it stopped finding new ones once it was full.)
    const size_t total = client->GetChatCount(), first = total - chat.size();
    if (total < chatSeen) chatSeen = 0;
    chatSeen = std::max(chatSeen, first);
    for (; chatSeen < total; chatSeen++)
        if (chat[chatSeen - first].text.rfind(mark, 0) == 0) {
            toastText = chat[chatSeen - first].text.substr(mark.size());
            toastUntil = Client::Now() + 4.0;
            audio->PlayHere("craft", 0.7f);
        }
    const double left = toastUntil - Client::Now();
    if (left <= 0.0 || toastText.empty()) return;
    const float fade = static_cast<float>(std::min(1.0, std::min(left, 4.0 - left) * 3.0));
    ui::DrawList* draw = ui::GetForegroundDrawList();
    const ui::Viewport* viewport = ui::GetMainViewport();
    const std::string title = "Achievement unlocked";
    const ui::Vec2 textSize = ui::CalcTextSize(toastText.c_str()), titleSize = ui::CalcTextSize(title.c_str());
    const float width = std::max(textSize.x, titleSize.x) + 40.0f, cx = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
    const float y = viewport->WorkPos.y + 62.0f - (1.0f - fade) * 20.0f;
    const ui::Vec2 lo(cx - width * 0.5f, y), hi(cx + width * 0.5f, y + titleSize.y + textSize.y + 18.0f);
    draw->AddRectFilled(lo, hi, UI_COL32(30, 24, 8, static_cast<int>(210 * fade)), 6.0f);
    draw->AddRect(lo, hi, UI_COL32(240, 200, 80, static_cast<int>(255 * fade)), 6.0f, 0, 2.0f);
    draw->AddText(ui::Vec2(cx - titleSize.x * 0.5f, y + 6.0f), UI_COL32(255, 214, 90, static_cast<int>(255 * fade)), title.c_str());
    draw->AddText(ui::Vec2(cx - textSize.x * 0.5f, y + 10.0f + titleSize.y), UI_COL32(255, 255, 255, static_cast<int>(240 * fade)), toastText.c_str());
}

void Application::DrawCompass() {
    // A strip across the top of the screen showing the way you face: N, E, S and W
    // (north is -z) with ticks between, sliding as you turn; under it, where you are
    // and the time of day.
    if (!client->IsWelcomed()) return;
    ui::DrawList* draw = ui::GetForegroundDrawList();
    const ui::Viewport* viewport = ui::GetMainViewport();
    const float width = 360.0f, cx = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, top = viewport->WorkPos.y + 10.0f;
    const float bearing = std::fmod(camera.GetYaw() + 90.0f + 720.0f, 360.0f); // degrees clockwise from north
    draw->AddRectFilled(ui::Vec2(cx - width * 0.5f, top), ui::Vec2(cx + width * 0.5f, top + 26.0f), UI_COL32(0, 0, 0, 90), 4.0f);
    draw->PushClipRect(ui::Vec2(cx - width * 0.5f, top), ui::Vec2(cx + width * 0.5f, top + 26.0f), true);
    constexpr float kPixelsPerDegree = 2.0f; // 180 degrees across
    for (int deg = 0; deg < 360; deg += 15) {
        float offset = std::fmod(deg - bearing + 540.0f, 360.0f) - 180.0f;
        const float x = cx + offset * kPixelsPerDegree;
        if (std::abs(offset) > width * 0.5f / kPixelsPerDegree + 10.0f) continue;
        const char* label = deg == 0 ? "N" : deg == 90 ? "E" : deg == 180 ? "S" : deg == 270 ? "W" : nullptr;
        if (label) {
            const ui::Vec2 size = ui::CalcTextSize(label);
            draw->AddText(ui::Vec2(x - size.x * 0.5f, top + 5.0f), deg == 0 ? UI_COL32(255, 110, 100, 255) : UI_COL32(255, 255, 255, 235), label);
        }
        else draw->AddLine(ui::Vec2(x, top + (deg % 45 == 0 ? 7.0f : 11.0f)), ui::Vec2(x, top + 19.0f), UI_COL32(255, 255, 255, deg % 45 == 0 ? 200 : 120), 1.5f);
    }
    draw->PopClipRect();
    draw->AddTriangleFilled(ui::Vec2(cx - 5.0f, top + 26.0f), ui::Vec2(cx + 5.0f, top + 26.0f), ui::Vec2(cx, top + 20.0f), UI_COL32(255, 230, 120, 255));
    // Where, and when (the day runs from sunrise at 06:00).
    const glm::vec3 feet = player.GetFeetPosition();
    const float hours = std::fmod(client->GetTimeOfDay() * 24.0f + 6.0f, 24.0f);
    const bool day = client->GetTimeOfDay() < 0.5f;
    char line[96];
    std::snprintf(line, sizeof line, "%d, %d, %d    %s %02d:%02d", static_cast<int>(std::floor(feet.x)), static_cast<int>(std::floor(feet.y)),
                  static_cast<int>(std::floor(feet.z)), day ? "sun" : "moon", static_cast<int>(hours), static_cast<int>(std::fmod(hours, 1.0f) * 60.0f));
    const ui::Vec2 size = ui::CalcTextSize(line);
    draw->AddText(ui::Vec2(cx - size.x * 0.5f + 1.0f, top + 31.0f), UI_COL32(0, 0, 0, 200), line);
    draw->AddText(ui::Vec2(cx - size.x * 0.5f, top + 30.0f), day ? UI_COL32(255, 244, 200, 235) : UI_COL32(190, 210, 255, 235), line);
}

void Application::DrawNameTags(const glm::mat4& viewProjection) {
    // Each other player's name over their head: within 40 blocks, in view, and not
    // behind a wall (the line from the eye to the tag is clear), fading with distance.
    ui::DrawList* draw = ui::GetBackgroundDrawList();
    const ui::Viewport* viewport = ui::GetMainViewport();
    const glm::vec3 eye = camera.GetPosition();
    constexpr float kRange = 40.0f;
    for (const auto& other : client->GetRemotePlayers()) {
        if (other.name.empty() || (other.flags & net::kFlagCrouching)) continue; // (crouching hides your name)
        const glm::vec3 tag = other.feet + glm::vec3(0.0f, 2.15f, 0.0f);
        const glm::vec3 toTag = tag - eye;
        const float distance = glm::length(toTag);
        if (distance > kRange || distance < 0.5f) continue;
        const glm::vec4 clip = viewProjection * glm::vec4(tag, 1.0f);
        if (clip.w <= 0.1f) continue; // behind the camera
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        if (std::abs(ndc.x) > 1.1f || std::abs(ndc.y) > 1.1f) continue;
        if (auto hit = Raycast(*world, eye, toTag / distance, distance - 0.3f)) continue; // behind a wall
        const ui::Vec2 at(viewport->WorkPos.x + (ndc.x * 0.5f + 0.5f) * viewport->WorkSize.x,
                        viewport->WorkPos.y + (0.5f - ndc.y * 0.5f) * viewport->WorkSize.y);
        const float fade = std::clamp((kRange - distance) / 10.0f, 0.0f, 1.0f);
        const ui::Vec2 size = ui::CalcTextSize(other.name.c_str());
        const ui::Vec2 lo(at.x - size.x * 0.5f - 5.0f, at.y - size.y - 4.0f), hi(at.x + size.x * 0.5f + 5.0f, at.y + 2.0f);
        draw->AddRectFilled(lo, hi, UI_COL32(0, 0, 0, static_cast<int>(120 * fade)), 3.0f);
        draw->AddText(ui::Vec2(lo.x + 5.0f, lo.y + 3.0f), UI_COL32(255, 255, 255, static_cast<int>(235 * fade)), other.name.c_str());
    }
}

void Application::DrawHealth() {
    if (!client->IsWelcomed()) return;
    ui::DrawList* draw = ui::GetForegroundDrawList();
    const ui::Viewport* viewport = ui::GetMainViewport();

    // A red flash when hurt, fading over half a second.
    if (client->IsCreative()) return; // nothing hurts in creative

    // Running out of air darkens the edges of the screen (blue under water), more as
    // it goes, with a slow pulse once it's gone.
    const int air = client->GetAir(), maxAir = std::max(1, client->GetMaxAir());
    const Breathing::Cause cause = client->GetAirCause();
    const float airLeft = static_cast<float>(air) / maxAir;
    if (cause != Breathing::Cause::None && airLeft < 0.6f) {
        float strength = (0.6f - airLeft) / 0.6f;
        if (air == 0) strength = 0.85f + 0.15f * std::sin(static_cast<float>(Client::Now()) * 4.0f);
        const ui::Vec2 lo = viewport->WorkPos, hi(lo.x + viewport->WorkSize.x, lo.y + viewport->WorkSize.y);
        const bool water = cause == Breathing::Cause::Water;
        const int r = water ? 0 : 10, g = water ? 20 : 8, b = water ? 60 : 12;
        const ui::Color edge = UI_COL32(r, g, b, static_cast<int>(230 * strength)), clear = UI_COL32(r, g, b, 0);
        const float band = std::min(viewport->WorkSize.x, viewport->WorkSize.y) * (0.15f + 0.3f * strength);
        draw->AddRectFilledMultiColor(lo, ui::Vec2(hi.x, lo.y + band), edge, edge, clear, clear);
        draw->AddRectFilledMultiColor(ui::Vec2(lo.x, hi.y - band), hi, clear, clear, edge, edge);
        draw->AddRectFilledMultiColor(lo, ui::Vec2(lo.x + band, hi.y), edge, clear, clear, edge);
        draw->AddRectFilledMultiColor(ui::Vec2(hi.x - band, lo.y), hi, clear, edge, edge, clear);
    }

    const float sinceHurt = static_cast<float>(Client::Now() - client->GetLastHurt());
    if (sinceHurt < 0.5f) {
        const int alpha = static_cast<int>(110.0f * (1.0f - sinceHurt / 0.5f));
        draw->AddRectFilled(viewport->WorkPos, ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
                            UI_COL32(200, 20, 20, alpha));
    }
    if (player.IsFlying()) return; // no-clip: nothing can hurt you

    // Hearts above the hotbar's left end: full, half or empty.
    constexpr float kSize = 9.0f, kGap = 4.0f;
    const int hearts = (client->GetMaxHealth() + 1) / 2, health = client->GetHealth();
    const float hotbarWidth = kHotbarSlots * 52.0f + (kHotbarSlots - 1) * 4.0f + 16.0f;
    const float x0 = viewport->WorkPos.x + (viewport->WorkSize.x - hotbarWidth) * 0.5f + 6.0f;
    const float y = viewport->WorkPos.y + viewport->WorkSize.y - 12.0f - 98.0f;
    auto heart = [&](float cx, float cy, ui::Color color) {
        // Two lobes and a point.
        draw->AddCircleFilled(ui::Vec2(cx - kSize * 0.45f, cy), kSize * 0.55f, color);
        draw->AddCircleFilled(ui::Vec2(cx + kSize * 0.45f, cy), kSize * 0.55f, color);
        draw->AddTriangleFilled(ui::Vec2(cx - kSize, cy + kSize * 0.15f), ui::Vec2(cx + kSize, cy + kSize * 0.15f), ui::Vec2(cx, cy + kSize * 1.1f), color);
    };
    const bool blink = sinceHurt < 0.6f && static_cast<int>(sinceHurt * 10.0f) % 2 == 0;
    for (int i = 0; i < hearts; i++) {
        const float cx = x0 + kSize + i * (2.0f * kSize + kGap);
        heart(cx + 1.5f, y + 1.5f, UI_COL32(0, 0, 0, 160));       // shadow
        heart(cx, y, UI_COL32(60, 50, 50, 220));                    // empty
        const int left = health - 2 * i;
        const ui::Color red = blink ? UI_COL32(255, 200, 200, 255) : UI_COL32(220, 30, 40, 255);
        if (left >= 2) heart(cx, y, red);
        else if (left == 1) {
            draw->PushClipRect(ui::Vec2(cx - kSize * 1.2f, y - kSize), ui::Vec2(cx, y + kSize * 1.2f), true); // left half
            heart(cx, y, red);
            draw->PopClipRect();
        }
    }

    // Food above the hotbar's right end, right to left: a drumstick is two points.
    const float right = viewport->WorkPos.x + (viewport->WorkSize.x + hotbarWidth) * 0.5f - 6.0f;
    {
        const int sticks = (client->GetMaxFood() + 1) / 2, food = client->GetFood();
        // Hungry: the row shivers a little, so it's noticed.
        const float shiver = food <= Hunger::kRegenFood / 2 ? std::sin(static_cast<float>(Client::Now()) * 30.0f) * 0.8f : 0.0f;
        auto drumstick = [&](float cx, float cy, ui::Color meat, ui::Color bone) {
            // The meat, upper left; the bone sticking out to the lower right with a knob.
            draw->AddLine(ui::Vec2(cx, cy), ui::Vec2(cx + kSize * 0.8f, cy + kSize * 0.8f), bone, kSize * 0.35f);
            draw->AddCircleFilled(ui::Vec2(cx + kSize * 0.85f, cy + kSize * 0.6f), kSize * 0.22f, bone);
            draw->AddCircleFilled(ui::Vec2(cx + kSize * 0.6f, cy + kSize * 0.85f), kSize * 0.22f, bone);
            draw->AddCircleFilled(ui::Vec2(cx - kSize * 0.2f, cy - kSize * 0.2f), kSize * 0.7f, meat);
        };
        for (int i = 0; i < sticks; i++) {
            const float cx = right - kSize * 1.4f - i * (2.0f * kSize + kGap), cy = y + shiver * ((i % 2) ? 1.0f : -1.0f);
            drumstick(cx + 1.5f, cy + 1.5f, UI_COL32(0, 0, 0, 160), UI_COL32(0, 0, 0, 160)); // shadow
            drumstick(cx, cy, UI_COL32(60, 45, 35, 220), UI_COL32(70, 65, 60, 220));          // empty
            const int left = food - 2 * i;
            const ui::Color meat = UI_COL32(176, 96, 44, 255), bone = UI_COL32(235, 225, 205, 255);
            if (left >= 2) drumstick(cx, cy, meat, bone);
            else if (left == 1) {
                draw->PushClipRect(ui::Vec2(cx, cy - kSize * 1.2f), ui::Vec2(cx + kSize * 1.4f, cy + kSize * 1.4f), true); // the bone half
                drumstick(cx, cy, meat, bone);
                draw->PopClipRect();
            }
        }
    }

    // An oxygen tank (worn): a gauge over the food while it's in use or filling.
    if ((client->GetArmour() & Armour::Bit(ArmourPiece::OxygenTank)) && (client->OnTank() || client->GetTank() < 0.999f)) {
        const float w = 9.0f * (2.0f * kSize + kGap) - kGap, h = 8.0f;
        const float y = viewport->WorkPos.y + viewport->WorkSize.y - 12.0f - 98.0f - (2.0f * kSize + 8.0f) * (cause != Breathing::Cause::None || air < maxAir ? 2.0f : 1.0f);
        const ui::Vec2 lo(right - w, y - h * 0.5f), hi(right, y + h * 0.5f);
        draw->AddRectFilled(ui::Vec2(lo.x - 2, lo.y - 2), ui::Vec2(hi.x + 2, hi.y + 2), UI_COL32(0, 0, 0, 160), 3.0f);
        const float fill = std::clamp(client->GetTank(), 0.0f, 1.0f);
        const ui::Color colour = fill < 0.25f ? UI_COL32(240, 90, 60, 255) : UI_COL32(120, 220, 230, 255);
        draw->AddRectFilled(lo, ui::Vec2(lo.x + w * fill, hi.y), colour, 2.0f);
        char label[48];
        std::snprintf(label, sizeof label, client->OnTank() ? "On the tank: %d%%" : "Tank filling: %d%%", static_cast<int>(fill * 100.0f + 0.5f));
        const ui::Vec2 size = ui::CalcTextSize(label);
        draw->AddText(ui::Vec2(right - size.x, lo.y - size.y - 3.0f), UI_COL32(220, 245, 250, 255), label);
    }

    // Air bubbles over the food, right to left, while anything takes your breath or
    // it's coming back; a bubble is two units of air.
    if (cause != Breathing::Cause::None || air < maxAir) {
        const int bubbles = (maxAir + 1) / 2;
        const float y = viewport->WorkPos.y + viewport->WorkSize.y - 12.0f - 98.0f - (2.0f * kSize + 8.0f);
        for (int i = 0; i < bubbles; i++) {
            const float cx = right - kSize - i * (2.0f * kSize + kGap);
            const int left = air - 2 * i;
            if (left <= 0) {
                draw->AddCircleFilled(ui::Vec2(cx, y + 2.0f), kSize * 0.75f, UI_COL32(20, 30, 45, 150)); // gone
                draw->AddCircle(ui::Vec2(cx, y + 2.0f), kSize * 0.75f, UI_COL32(140, 180, 220, 170), 0, 1.5f);
                continue;
            }
            const float radius = kSize * (left >= 2 ? 0.8f : 0.5f); // half a bubble is a small one
            draw->AddCircleFilled(ui::Vec2(cx + 1.5f, y + 3.5f), radius, UI_COL32(0, 0, 0, 140));
            draw->AddCircleFilled(ui::Vec2(cx, y + 2.0f), radius, UI_COL32(90, 170, 255, 235));
            draw->AddCircle(ui::Vec2(cx, y + 2.0f), radius, UI_COL32(220, 240, 255, 255), 0, 1.5f);
            draw->AddCircleFilled(ui::Vec2(cx - radius * 0.35f, y + 2.0f - radius * 0.35f), radius * 0.25f, UI_COL32(255, 255, 255, 230)); // shine
        }
        if (cause != Breathing::Cause::None) {
            const char* label = CauseName(cause);
            const ui::Vec2 size = ui::CalcTextSize(label);
            const ui::Vec2 at(right - size.x, y - kSize - size.y - 4.0f);
            draw->AddText(ui::Vec2(at.x + 1.0f, at.y + 1.0f), UI_COL32(0, 0, 0, 220), label);
            draw->AddText(at, air == 0 ? UI_COL32(255, 120, 110, 255) : UI_COL32(200, 230, 255, 255), label);
        }
    }
}

void Application::OpenChat(char openedWith) {
    // While typing the view stays put and the cursor is free (to click in the box).
    cursorBeforeChat = captureCursor;
    SetCursorCaptured(false);
    chatOpen = true;
    chatOpenedWith = openedWith;
    chatOpenFrames = 0;
    chatHistoryAt = -1;
    std::snprintf(chatInput, sizeof chatInput, "%s", openedWith == '/' ? "/" : "");
}

void Application::CloseChat() {
    if (!chatOpen) return;
    chatOpen = false;
    SetCursorCaptured(cursorBeforeChat);
}

void Application::DrawChat() {
    constexpr double kShowSeconds = 10.0, kFadeSeconds = 2.0;
    constexpr size_t kLinesClosed = 8, kLinesOpen = 16;
    const auto& lines = client->GetChat();
    const double now = Client::Now();

    // Which lines to show: while typing, the last few; otherwise only recent ones, fading.
    size_t first = lines.size();
    const size_t limit = chatOpen ? kLinesOpen : kLinesClosed;
    while (first > 0 && lines.size() - first < limit && (chatOpen || now - lines[first - 1].time < kShowSeconds)) first--;
    if (first == lines.size() && !chatOpen) return;

    const ui::Viewport* viewport = ui::GetMainViewport();
    const float width = std::min(560.0f, viewport->WorkSize.x - 24.0f);
    // Above the hotbar and the hearts and air bubbles over it.
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + viewport->WorkSize.y - 160.0f), ui::Cond_Always,
                            ui::Vec2(0.0f, 1.0f));
    ui::SetNextWindowSize(ui::Vec2(width, 0.0f));
    ui::SetNextWindowBgAlpha(chatOpen ? 0.7f : 0.55f);
    ui::PushStyleColor(ui::Col_WindowBg, ui::Vec4(0.0f, 0.0f, 0.0f, 1.0f)); // black, so text reads over bright terrain
    ui::PushStyleColor(ui::Col_Border, ui::Vec4(0.0f, 0.0f, 0.0f, 0.0f));
    const int flags = ui::WindowFlags_NoDecoration | ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove |
                                   ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoFocusOnAppearing | ui::WindowFlags_NoNav;
    const bool visible = ui::Begin("Chat", nullptr, flags);
    ui::PopStyleColor(2);
    if (!visible) {
        ui::End();
        return;
    }
    ui::PushTextWrapPos(width - 16.0f);
    for (size_t i = first; i < lines.size(); i++) {
        const auto& line = lines[i];
        ui::Vec4 color(1.0f, 1.0f, 1.0f, 1.0f);
        switch (line.kind) {
        case net::ChatMessage::Kind::Info: color = ui::Vec4(1.0f, 0.88f, 0.45f, 1.0f); break;
        case net::ChatMessage::Kind::Error: color = ui::Vec4(1.0f, 0.5f, 0.45f, 1.0f); break;
        case net::ChatMessage::Kind::Private: color = ui::Vec4(0.82f, 0.65f, 1.0f, 1.0f); break;
        default: break;
        }
        if (!chatOpen) color.w = static_cast<float>(std::clamp((kShowSeconds - (now - line.time)) / kFadeSeconds, 0.0, 1.0));
        ui::PushStyleColor(ui::Col_Text, color);
        ui::TextUnformatted(line.text.c_str());
        ui::PopStyleColor();
    }
    ui::PopTextWrapPos();

    if (chatOpen) {
        if (chatOpenFrames == 0) ui::SetKeyboardFocusHere();
        ui::SetNextItemWidth(-1.0f);
        // Just opened: the key that opened it may also arrive as a typed character (drop
        // the 't', keep a single '/'). Up and Down step back and forward through what was
        // typed before, the newest first; past the newest is the line being typed, as it
        // was left.
        const int count = static_cast<int>(chatHistory.size()), was = chatHistoryAt;
        if (ui::IsKeyPressed(ui::Key::Up) && count > 0) {
            if (was == -1) chatDraft = chatInput, chatHistoryAt = count - 1;
            else if (was > 0) chatHistoryAt = was - 1;
        }
        else if (ui::IsKeyPressed(ui::Key::Down) && was != -1)
            chatHistoryAt = was + 1 < count ? was + 1 : -1;
        if (chatHistoryAt != was) {
            const std::string& text = chatHistoryAt == -1 ? chatDraft : chatHistory[static_cast<size_t>(chatHistoryAt)];
            std::snprintf(chatInput, sizeof chatInput, "%s", text.c_str());
        }
        const bool opening = chatOpenFrames == 0; // (Enter opening it mustn't also send it, empty)
        const bool send = ui::InputText("##chat", chatInput, sizeof chatInput, ui::InputTextFlags_EnterReturnsTrue) && !opening;
        if (chatOpenFrames < 3) {
            const std::string text = chatInput;
            if (chatOpenedWith == 't' && (text == "t" || text == "T")) chatInput[0] = '\0';
            if (chatOpenedWith == '/' && text == "//") chatInput[1] = '\0';
        }
        chatOpenFrames++;
        if (send) {
            RememberChat(chatInput);
            client->SendChat(chatInput);
            CloseChat();
        }
    }
    ui::End();
}

void Application::DrawStatus() {
    std::string text;
    if (!client->IsConnected() && !client->GetDisconnectReason().empty()) text = "Disconnected: " + client->GetDisconnectReason();
    else if (!client->IsWelcomed()) text = "Connecting to " + serverLabel + "...";
    else if (!player.IsSpawned() && !player.IsFlying()) text = "Loading terrain...";
    if (text.empty()) return;

    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 40.0f), ui::Cond_Always,
                            ui::Vec2(0.5f, 0.0f));
    ui::SetNextWindowBgAlpha(0.6f);
    const int flags = ui::WindowFlags_NoDecoration | ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove |
                                   ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoFocusOnAppearing | ui::WindowFlags_NoNav;
    if (ui::Begin("Status", nullptr, flags)) ui::TextUnformatted(text.c_str());
    ui::End();
}

void Application::HandleContinuousInput(float deltaTime) {
    auto down = [&](int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
    if (!joined) return;

    // The player drops onto the terrain once the chunks under it have loaded.
    if (!player.IsSpawned() && !player.IsFlying()) player.Spawn(*world);

    // No-clip is the server's to switch (/noclip): follow it. Creative flight is ours
    // (double-tapping jump), and stops outside creative.
    const bool noclip = client->GetAbilities() & net::kAbilityNoclip;
    if (!client->IsCreative()) creativeFlight = false;
    if (autoFly && client->IsCreative()) creativeFlight = true;
    const bool wantFly = noclip || creativeFlight;
    if (wantFly != player.IsFlying() || (noclip && player.IsFlyingSolid())) player.SetFlying(*world, wantFly, !noclip);

    PlayerInput input;
    if (!chatOpen && !inventoryOpen && !paused) {
    auto key = [&](Action a) { return down(settings.Key(a)); };
    input.move = glm::vec2((key(Action::Right) ? 1.0f : 0.0f) - (key(Action::Left) ? 1.0f : 0.0f),
                           (key(Action::Forward) ? 1.0f : 0.0f) - (key(Action::Back) ? 1.0f : 0.0f));
    input.jump = key(Action::Jump) || (player.IsFlying() && key(Action::FlyUp));
    input.descend = key(Action::FlyDown) || key(Action::Crouch);
    input.crouch = key(Action::Crouch) && !player.IsFlying(); // (flying, crouch goes down)
    // A gamepad: the left stick walks, A jumps, B crouches.
    GLFWgamepadstate pad{};
    if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &pad)) {
        glm::vec2 stick(pad.axes[GLFW_GAMEPAD_AXIS_LEFT_X], -pad.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
        if (glm::length(stick) > 0.2f) input.move = glm::clamp(input.move + stick, glm::vec2(-1.0f), glm::vec2(1.0f));
        input.jump |= pad.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS;
        input.crouch |= pad.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS && !player.IsFlying();
        input.descend |= pad.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS;
    }
    // In a boat, Ctrl gets out (and doesn't crouch).
    if (player.IsBoating() && input.crouch) {
        player.SetBoating(false);
        input.crouch = false;
    }
    // Short of breath (thin or stale air, half the air gone): too winded to sprint.
    const Breathing::Cause cause = client->GetAirCause();
    const bool winded = (cause == Breathing::Cause::ThinAir || cause == Breathing::Cause::StaleAir) && client->GetAir() < Breathing::kShortOfBreath;
    input.sprint = (key(Action::Sprint) || pad.buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] == GLFW_PRESS) && !winded;
    }
    if (autoFly && player.IsFlying()) input.move = glm::vec2(0.0f, 1.0f), input.sprint = true;
    input.yaw = camera.GetYaw();
    input.lookDirection = camera.GetDirection();
    player.Update(*world, input, deltaTime);
    if (creativeFlight && !player.IsFlying()) creativeFlight = false; // (landed)

    // Our position is ours to predict; the server checks it loosely and corrects us if
    // it disagrees. Nothing is sent until we've landed on the terrain.
    if (player.IsSpawned() || player.IsFlying()) {
        uint8_t flags = 0;
        if (player.IsFlying()) flags |= net::kFlagFlying;
        if (player.IsOnGround()) flags |= net::kFlagOnGround;
        if (player.IsSwimming()) flags |= net::kFlagSwimming;
        if (player.IsCrouching()) flags |= net::kFlagCrouching;
        if (player.IsBoating()) flags |= net::kFlagBoat;
        // What's in hand, for others to see (a survival player's empty slot holds nothing).
        const Tool heldTool = SelectedTool();
        const ItemId heldItem = Items::IsStackable(SelectedItem()) ? SelectedItem() : Items::kNone;
        client->SendPlayerState(player.GetFeetPosition(), player.GetVelocity(), camera.GetYaw(), camera.GetDirectionAngles().x, flags, heldTool,
                                heldItem);
    }

    UpdateMining();
    UpdateThrow();

    // Laser: carve a tunnel along the view ray while L is held.
    if (captureCursor && !chatOpen && (client->GetAbilities() & net::kAbilityOperator) && glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
        client->Carve(TraceBlocks(*world, camera.GetPosition(), camera.GetDirection(), kLaserRange), kLaserRadius);
    }
}

void Application::DrawHotbar() {
    SeedCreativeHotbar();
    constexpr int kSlots = kHotbarSlots;
    constexpr float kIcon = 40.0f, kPad = 6.0f, kGap = 4.0f;
    const ui::Viewport* viewport = ui::GetMainViewport();
    const float barWidth = kSlots * (kIcon + 2 * kPad) + (kSlots - 1) * kGap;

    // A borderless strip along the bottom centre of the screen.
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y - 12.0f),
                            ui::Cond_Always, ui::Vec2(0.5f, 1.0f));
    ui::SetNextWindowBgAlpha(0.45f);
    ui::PushStyleVar(ui::StyleVar_WindowPadding, ui::Vec2(8.0f, 6.0f));
    ui::PushStyleVar(ui::StyleVar_ItemSpacing, ui::Vec2(kGap, 4.0f));
    ui::PushStyleVar(ui::StyleVar_WindowRounding, 6.0f);
    const int flags = ui::WindowFlags_NoDecoration | ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove |
                                   ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoFocusOnAppearing | ui::WindowFlags_NoNav |
                                   ui::WindowFlags_NoBringToFrontOnFocus;
    if (ui::Begin("Hotbar", nullptr, flags)) {
        // The selected item's name, centred above the slots, with the height building
        // blocks are placed at (V); reddish if it can't be used (a tool not made yet,
        // or a block we haven't enough of).
        const BlockType selected = SelectedBlock();
        const Tool selectedTool = SelectedTool();
        const bool survival = !client->IsCreative();
        std::string label = SelectedItem() != Items::kNone ? ItemName(SelectedItem()) : "Empty hand";
        if (selectedTool == Tool::Chisel) label += std::string(" - ") + ChiselStepName(chiselStep) + " a stroke (V)";
        if (selectedTool == Tool::Shovel) label += " - flattens " + std::to_string(2 * flattenRadius + 1) + "x" + std::to_string(2 * flattenRadius + 1) + " (V)";
        if (selectedTool == Tool::None && Block::CanShape(selected))
            label += std::string(" - ") + HeightName(placeHeight) + (placeHeight < 8 ? " height" : " block") + " (V)";
        const bool affordable = SelectedItem() == Items::kNone || !Items::IsBlock(SelectedItem()) ||
                                client->CanAfford(selected, Block::CanShape(selected) ? placeHeight : 8);
        const bool affordableShown = affordable;
        const float nameWidth = ui::CalcTextSize(label.c_str()).x;
        ui::SetCursorPosX(ui::GetCursorPosX() + std::max(0.0f, (barWidth - nameWidth) * 0.5f));
        ui::PushStyleColor(ui::Col_Text, affordableShown ? ui::Vec4(1.0f, 1.0f, 1.0f, 1.0f) : ui::Vec4(1.0f, 0.55f, 0.5f, 1.0f));
        ui::TextUnformatted(label.c_str());
        ui::PopStyleColor();

        const TextureAtlas& atlas = renderer->GetAtlas();
        const ui::TextureId atlasTexture = static_cast<ui::TextureId>(static_cast<uintptr_t>(atlas.GetTexture()));
        ui::DrawList* draw = ui::GetWindowDrawList();
        for (int i = 0; i < kSlots; i++) {
            if (i > 0) ui::SameLine();
            const bool selected = i == selectedSlot;
            const ui::Vec2 slotMin = ui::GetCursorScreenPos();
            const ui::Vec2 slotMax(slotMin.x + kIcon + 2 * kPad, slotMin.y + kIcon + 2 * kPad);

            // Clicking a slot selects it (when the cursor is released with Tab).
            ui::PushID(i);
            if (ui::InvisibleButton("slot", ui::Vec2(slotMax.x - slotMin.x, slotMax.y - slotMin.y))) SelectSlot(i);
            const bool hovered = ui::IsItemHovered();
            ui::PopID();

            draw->AddRectFilled(slotMin, slotMax, selected ? UI_COL32(255, 255, 255, 60) : hovered ? UI_COL32(255, 255, 255, 30) : UI_COL32(0, 0, 0, 70), 4.0f);
            // What's in the slot (survival: how many, in the bottom right).
            DrawStackOn(draw, atlas, atlasTexture, slotMin, slotMax, kPad, StackIn(SlotBox::Player, i), false, survival);
            draw->AddRect(slotMin, slotMax, selected ? UI_COL32(255, 255, 255, 255) : UI_COL32(255, 255, 255, 60), 4.0f, 0, selected ? 3.0f : 1.0f);
            // Number key in the corner (1-9, then 0; later slots are picked with the wheel).
            char key[2] = { static_cast<char>(i < 9 ? '1' + i : i == 9 ? '0' : 0), 0 };
            draw->AddText(ui::Vec2(slotMin.x + 5.0f, slotMin.y + 3.0f), UI_COL32(0, 0, 0, 200), key); // shadow, readable on light textures
            draw->AddText(ui::Vec2(slotMin.x + 4.0f, slotMin.y + 2.0f), UI_COL32(255, 255, 255, selected ? 255 : 190), key);
        }
    }
    ui::End();
    ui::PopStyleVar(3);
}

void Application::DrawDebugWindow(float deltaTime) {
    frameSampleTimer += deltaTime;
    if (frameSampleTimer > 0.01f) {
        frameTimes[frameTimeIndex] = deltaTime * 1000.0f;
        frameTimeIndex = (frameTimeIndex + 1) % kFrameHistory;
        frameSampleTimer = 0.0f;
    }

    if (ui::Begin("Debug", &showDebug)) {
        const glm::vec3 pos = camera.GetPosition();
        const glm::vec2 angles = camera.GetDirectionAngles();

        ui::Checkbox("VSync", &vsync);
        ui::Checkbox("Wireframe", &wireframe);
        ui::Checkbox("Ambient occlusion", &ambientOcclusion);
        {
            const float t = client->GetTimeOfDay();
            const int minutes = static_cast<int>((t * 24.0f + 6.0f) * 60.0f) % (24 * 60); // sunrise at 06:00
            ui::Text("Time %02d:%02d, rain %.0f%%%s", minutes / 60, minutes % 60, client->GetRain() * 100.0f,
                        sheltered ? ", sheltered" : "");
        }
        ui::Text("Placing: %s", BlockName(SelectedBlock()));
        ui::Text("Position %.3f, %.3f, %.3f", pos.x, pos.y, pos.z);
        if (!(client->GetSpawnFlags() & net::kWelcomeEmptyWorld)) {
            // The world generator is deterministic, so the biome can be worked out here.
            static const TerrainGenerator climate;
            const int bx = static_cast<int>(std::floor(pos.x)), bz = static_cast<int>(std::floor(pos.z));
            const int row = climate.ColumnAt(bx, bz).surfaceRow;
            const auto c = climate.ClimateAt(bx, bz, row);
            ui::Text("Biome: %s (temperature %.2f, humidity %.2f)", BiomeName(TerrainGenerator::Classify(c)), c.temperature, c.humidity);
        }
        const glm::vec3 velocity = player.GetVelocity();
        ui::Text("%s%s, velocity %.2f, %.2f, %.2f", player.IsFlying() ? "Flying" : player.IsSwimming() ? "Swimming" : "Walking",
                    player.IsFlying() || player.IsSwimming() ? "" : player.IsOnGround() ? " (on ground)" : " (airborne)",
                    velocity.x, velocity.y, velocity.z);
        ui::Text("Angles %.3f, %.3f", angles.x, angles.y);
        ui::Text("Frame %.3f ms (%.1f FPS)", deltaTime * 1000.0f, deltaTime > 0.0f ? 1.0f / deltaTime : 0.0f);
        ui::PlotLines("Frame Time (ms)", frameTimes.data(), kFrameHistory, frameTimeIndex);
        ui::Text("Chunks: %zu received, %zu tracked, %d alive", world->GetLoadedChunkCount(), world->GetChunkCount(), Chunk::GetLiveCount());
        const Client::Stats net = client->GetStats();
        ui::Text("Server: %s, player %u, ping %u ms", serverLabel.c_str(), client->GetPlayerId(), net.roundTripMs);
        ui::Text("Received %.1f MB: %u chunks, %u cell updates, %u light updates", net.bytesReceived / (1024.0 * 1024.0),
                    net.chunksReceived, net.deltasReceived, net.lightUpdates);
        ui::Text("Other players: %zu", client->GetRemotePlayers().size());

        const TextureAtlas& atlas = renderer->GetAtlas();
        if (ui::CollapsingHeader("Texture atlas")) {
            glm::ivec2 size = atlas.GetSize();
            ui::Text("%zu textures, %dx%d, %dpx tiles (R to reload)", atlas.GetNames().size() - 1, size.x, size.y, atlas.GetTileSize());
            const float scale = 128.0f / std::max(size.x, size.y);
            ui::Image(static_cast<ui::TextureId>(static_cast<uintptr_t>(atlas.GetTexture())), ui::Vec2(size.x * scale, size.y * scale));
        }

        const float previewScale = 0.25f;
        ui::Image(static_cast<ui::TextureId>(static_cast<uintptr_t>(renderer->GetSceneTexture())),
                     ui::Vec2(renderer->GetWidth() * previewScale, renderer->GetHeight() * previewScale),
                     ui::Vec2(0, 1), ui::Vec2(1, 0));
    }
    ui::End();
}

void Application::OnKey(int key, int action) {
    if (action != GLFW_PRESS) return;
    // Waiting for a key to bind (in the settings): this is it (Esc keeps the old one).
    if (rebinding >= 0) {
        if (key != GLFW_KEY_ESCAPE) {
            settings.keys[static_cast<size_t>(rebinding)] = key;
            settings.Save(kSettingsFile);
        }
        rebinding = -1;
        return;
    }

    // The title screen: Esc goes back a page.
    if (!client) {
        if (key == GLFW_KEY_ESCAPE && page != Page::Title) page = Page::Title;
        return;
    }
    // Paused: Esc goes back (from the settings) or resumes.
    if (paused) {
        if (key == GLFW_KEY_ESCAPE) {
            if (pauseSettings) pauseSettings = false;
            else if (client->IsConnected()) {
                paused = false;
                SetCursorCaptured(true);
            }
        }
        return;
    }

    // While typing, keys belong to the chat line (the UI gets them too); Esc closes it.
    if (chatOpen) {
        if (key == GLFW_KEY_ESCAPE) CloseChat();
        return;
    }
    // The inventory takes its key (Tab) and Esc (to close) and the number keys (to pick a slot).
    if (inventoryOpen) {
        if (ui::GetIO().WantTextInput && key != GLFW_KEY_ESCAPE) return; // (typing in a box: the keys are letters)
        if ((key == settings.Key(Action::Inventory) && !signMode && !signalMode) || key == GLFW_KEY_ESCAPE) SetInventoryOpen(false); // (writing a sign, it's a letter)
        else if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) SelectSlot(key - GLFW_KEY_1);
        else if (key == GLFW_KEY_0) SelectSlot(9);
        return;
    }

    // The rebindable ones (see app/Bindings.h).
    const auto is = [&](Action a) { return key == settings.Key(a); };
    // Jump twice quickly in creative: fly (or stop flying).
    if (is(Action::Jump) && client->IsCreative() && !(client->GetAbilities() & net::kAbilityNoclip)) {
        const double now = glfwGetTime();
        if (now - lastJumpPress < 0.3) {
            creativeFlight = !creativeFlight;
            lastJumpPress = -10.0;
        }
        else lastJumpPress = now;
    }
    if (is(Action::Inventory)) return SetInventoryOpen(true);
    if (is(Action::PlaceHeight)) {
        // Placement height: full, 3/4, 1/2, 1/4 (crouching, back up). With the chisel in
        // hand, how fine it cuts instead: an eighth, a quarter, a half (crouching: back).
        const bool shift = CrouchHeld();
        if (SelectedTool() == Tool::Shovel) {
            flattenRadius = shift ? (flattenRadius <= 1 ? net::kMaxFlattenRadius : flattenRadius - 1) : (flattenRadius >= net::kMaxFlattenRadius ? 1 : flattenRadius + 1);
            return;
        }
        if (SelectedTool() == Tool::Chisel) {
            constexpr int kSteps = static_cast<int>(std::size(Block::kChiselSteps));
            int i = 0;
            while (i < kSteps - 1 && Block::kChiselSteps[i] != chiselStep) i++;
            chiselStep = Block::kChiselSteps[(i + (shift ? kSteps - 1 : 1)) % kSteps];
            return;
        }
        placeHeight = shift ? (placeHeight >= 8 ? 2 : placeHeight + 2) : (placeHeight <= 2 ? 8 : placeHeight - 2);
        return;
    }
    if (is(Action::Drop)) {
        // Winds up while held; thrown when let go (UpdateThrow).
        const auto& slots = client->GetInventory().slots;
        if (selectedSlot >= 0 && selectedSlot < static_cast<int>(slots.size()) && !slots[selectedSlot].Empty()) {
            throwHeld = true;
            throwSince = glfwGetTime();
        }
        return;
    }
    if (is(Action::Chat)) return OpenChat('t');
    if (is(Action::Command)) return OpenChat('/');
    if (is(Action::Map)) {
        showMap = !showMap;
        return;
    }
    if (is(Action::Players)) {
        showPlayers = !showPlayers;
        return;
    }
    if (is(Action::Reload)) {
        renderer->ReloadShaders();
        renderer->ReloadTextures();
        LoadData();
        LoadBlockTextures();
        world->RebuildAllChunks();
        return;
    }
    switch (key) {
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
        OpenChat(0);
        break;
    case GLFW_KEY_ESCAPE:
        paused = true; // the pause menu: resume, settings, back to the title screen
        SetCursorCaptured(false);
        break;
    case GLFW_KEY_F1:
        wireframe = !wireframe;
        break;
    case GLFW_KEY_F2:
        showDebug = !showDebug;
        break;
    case GLFW_KEY_F3:
        ambientOcclusion = !ambientOcclusion;
        break;
    case GLFW_KEY_F4:
        showCompass = !showCompass;
        break;
    case GLFW_KEY_F12:
        screenshotRequested = true;
        break;
    default:
        if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) SelectSlot(key - GLFW_KEY_1);
        else if (key == GLFW_KEY_0) SelectSlot(9);
        break;
    }
}

void Application::HandleGamepad(float deltaTime) {
    GLFWgamepadstate pad{};
    if (!glfwJoystickIsGamepad(GLFW_JOYSTICK_1) || !glfwGetGamepadState(GLFW_JOYSTICK_1, &pad)) {
        padMining = false;
        return;
    }
    auto pressed = [&](int b) { return pad.buttons[b] == GLFW_PRESS && padWas[b] != GLFW_PRESS; };
    // Start pauses (and resumes); Y opens and closes the inventory.
    if (pressed(GLFW_GAMEPAD_BUTTON_START)) {
        if (paused) {
            if (client->IsConnected()) paused = pauseSettings = false, SetCursorCaptured(true);
        } else paused = true, SetCursorCaptured(false);
    }
    const bool playing = !paused && !chatOpen;
    if (playing && pressed(GLFW_GAMEPAD_BUTTON_Y)) SetInventoryOpen(!inventoryOpen);
    if (playing && !inventoryOpen) {
        // The right stick looks round.
        glm::vec2 look(pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
        if (glm::length(look) > 0.15f) {
            const glm::vec2 angles = camera.GetDirectionAngles();
            camera.SetLook(angles.y + look.x * settings.padLook * deltaTime, std::clamp(angles.x - look.y * settings.padLook * deltaTime, -89.0f, 89.0f));
        }
        // Bumpers: the hotbar slot before or after; Back: the map.
        if (pressed(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER)) SelectSlot((selectedSlot + 9) % 10);
        if (pressed(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER)) SelectSlot((selectedSlot + 1) % 10);
        if (pressed(GLFW_GAMEPAD_BUTTON_BACK)) showMap = !showMap;
        // Triggers: the right one mines and hits (held: keeps mining), the left one
        // places and uses, like the mouse buttons.
        const bool right = pad.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] > 0.4f, left = pad.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] > 0.4f;
        if (right && !padTriggers[1]) OnMouseButton(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS);
        if (left && !padTriggers[0]) OnMouseButton(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
        padMining = right;
        padTriggers[0] = left, padTriggers[1] = right;
    }
    else padMining = false;
    std::copy(std::begin(pad.buttons), std::end(pad.buttons), std::begin(padWas));
}

ItemId Application::SelectedItem() const {
    const auto& slots = client->GetInventory().slots;
    return selectedSlot < static_cast<int>(slots.size()) && !slots[selectedSlot].Empty() ? slots[selectedSlot].item : Items::kNone;
}

void Application::SeedCreativeHotbar() {
    // Once, when the server has said what we carry: a creative player with an empty
    // hotbar gets the usual blocks (or what they chose before the hotbar was the
    // inventory's, in hotbar.txt).
    if (hotbarSeeded || !client->IsWelcomed() || !client->HasInventory()) return;
    hotbarSeeded = true;
    if (!client->IsCreative()) return;
    const auto& slots = client->GetInventory().slots;
    for (int i = 0; i < kHotbarSlots && i < static_cast<int>(slots.size()); i++)
        if (!slots[i].Empty()) return;
    for (int i = 0; i < kHotbarSlots; i++) {
        const ItemId item = creativeHotbarTools[i] != Tool::None ? Items::OfTool(creativeHotbarTools[i], Items::kTiers - 1) : creativeHotbar[i];
        if (Items::Valid(item)) client->SetCreativeSlot(i, item, static_cast<int>(Items::MaxStack(item)));
    }
}

void Application::SelectSlot(int slot) {
    selectedSlot = ((slot % kHotbarSlots) + kHotbarSlots) % kHotbarSlots;
}

void Application::LoadHotbar() {
    // (Written before items had their own types: block numbers of the time.)
    std::ifstream file(kHotbarFile);
    for (int i = 0, type; i < kHotbarSlots && file >> type; i++) {
        const ItemId item = type >= 0 && type < 256 ? Legacy::ItemFromV16Block(static_cast<uint8_t>(type)) : Items::kNone;
        if (std::any_of(std::begin(ItemPalette::kItems), std::end(ItemPalette::kItems), [&](ItemId i) { return i == item; })) creativeHotbar[i] = item;
    }
    // A second line: the tool in each slot (0 none).
    for (int i = 0, tool; i < kHotbarSlots && file >> tool; i++)
        if (tool > 0 && tool < static_cast<int>(Tool::Count)) creativeHotbarTools[i] = static_cast<Tool>(tool);
}

void Application::LoadChatHistory() {
    chatHistory.clear();
    std::ifstream file(kChatHistoryFile);
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) chatHistory.push_back(line.substr(0, sizeof chatInput - 1));
    }
    if (chatHistory.size() > kChatHistoryMax) chatHistory.erase(chatHistory.begin(), chatHistory.end() - kChatHistoryMax);
}

void Application::RememberChat(const std::string& line) {
    // Nothing worth recalling in an empty line or a lone '/', and no repeats in a row.
    if (line.find_first_not_of(" /") == std::string::npos) return;
    if (!chatHistory.empty() && chatHistory.back() == line) return;
    chatHistory.push_back(line);
    if (chatHistory.size() > kChatHistoryMax) chatHistory.erase(chatHistory.begin());
    std::ofstream file(kChatHistoryFile, std::ios::trunc);
    for (const auto& l : chatHistory) file << l << '\n';
}

void Application::SetInventoryOpen(bool open) {
    if (open == inventoryOpen) return;
    inventoryOpen = open;
    heldStack.reset();
    draggingStack = false;
    if (!open && chestMode) {
        client->CloseChest();
        chestMode = chestSeenOpen = false;
    }
    if (!open) signMode = false, signalMode = false, tradeMode = false, machineMode = false;
    // The cursor is needed to click blocks; give it back to the game on closing.
    if (open) cursorBeforeInventory = captureCursor;
    captureCursor = open ? false : cursorBeforeInventory;
    glfwSetInputMode(window, GLFW_CURSOR, captureCursor ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    camera.ResetLook();
}

void Application::DrawTrade() {
    // Gone, or walked away from: closed.
    bool near = false;
    for (const auto& c : client->GetCreatures())
        if (c.id == tradeWith) near = glm::distance(c.feet, camera.GetPosition()) < Trades::kReach + 2.0f;
    if (!near) return SetInventoryOpen(false);
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ui::Cond_Always, ui::Vec2(0.5f, 0.5f));
    ui::SetNextWindowBgAlpha(0.88f);
    const int flags = ui::WindowFlags_NoCollapse | ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove |
                                   ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoResize;
    bool keepOpen = true;
    if (ui::Begin("Dwarf trader", &keepOpen, flags)) {
        const TextureAtlas& atlas = renderer->GetAtlas();
        const ui::TextureId texture = static_cast<ui::TextureId>(static_cast<uintptr_t>(atlas.GetTexture()));
        constexpr float kIcon = 32.0f;
        auto icon = [&](ItemId item) {
            glm::vec2 uv0, uv1;
            atlas.GetTileUV(ItemTile(atlas, item), uv0, uv1);
            ui::Image(texture, ui::Vec2(kIcon, kIcon), ui::Vec2(uv0.x, uv0.y), ui::Vec2(uv1.x, uv1.y));
            ui::SetItemTooltip("%s", ItemName(item).c_str());
        };
        ui::TextDisabled("\"Gold's what we deal in. Buying or selling?\"");
        const auto& offers = Trades::Offers();
        const auto& prices = client->GetTradePrices();
        for (size_t i = 0; i < offers.size(); i++) {
            const Trades::Offer& o = offers[i];
            if (i == 0) ui::SeparatorText("They sell");
            if (i == 5) ui::SeparatorText("They buy");
            if (i == 11) ui::SeparatorText("Barter");
            ui::PushID(static_cast<int>(i));
            const int price = i < prices.size() ? prices[i] : o.giveCount;
            const uint32_t have = client->GetResources(o.give) / Block::kUnitsPerBlock;
            const bool can = client->IsCreative() || have >= static_cast<uint32_t>(price);
            icon(o.give);
            ui::SameLine();
            ui::AlignTextToFramePadding();
            ui::TextColored(can ? ui::Vec4(1, 1, 1, 1) : ui::Vec4(1.0f, 0.5f, 0.45f, 1.0f), "%d %s", price, ItemName(o.give).c_str());
            if (price > o.giveCount) {
                ui::SameLine();
                ui::TextColored(ui::Vec4(1.0f, 0.75f, 0.3f, 1.0f), "(up from %d)", o.giveCount);
            }
            ui::SameLine(250.0f);
            ui::TextUnformatted("->");
            ui::SameLine();
            if (o.hire) {
                ui::TextUnformatted("a guard for ten minutes");
                ui::SetItemTooltip("Follows you and fights hostile dwarves (at most %d at once)", Trades::kMaxHired);
            } else {
                icon(o.get);
                ui::SameLine();
                ui::Text("%d %s", o.getCount, ItemName(o.get).c_str());
            }
            ui::SameLine(560.0f);
            ui::BeginDisabled(!can);
            if (ui::Button(o.hire ? "Hire" : "Trade")) client->Trade(tradeWith, static_cast<uint8_t>(i));
            ui::EndDisabled();
            ui::PopID();
        }
        ui::TextDisabled("You have what's in white.");
    }
    ui::End();
    if (!keepOpen) SetInventoryOpen(false);
}

void Application::DrawInventory() {
    if (!inventoryOpen) return;
    if (chestMode) return DrawChest();
    if (machineMode) return DrawMachine();
    if (signMode) return DrawSignEditor();
    if (signalMode) return DrawSignalEditor();
    if (tradeMode) return DrawTrade();
    constexpr float kIcon = 38.0f;
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f - kItemPanelWidth * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ui::Cond_Always, ui::Vec2(0.5f, 0.5f));
    ui::SetNextWindowBgAlpha(0.85f);
    const int flags = ui::WindowFlags_NoCollapse | ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove |
                                   ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoResize;
    bool open = true;
    hoveredSlot.reset();
    if (ui::Begin("Inventory", &open, flags)) {
        const bool survival = !client->IsCreative();
        const TextureAtlas& atlas = renderer->GetAtlas();
        const ui::TextureId texture = static_cast<ui::TextureId>(static_cast<uintptr_t>(atlas.GetTexture()));
        ui::PushTextWrapPos(ui::GetCursorPosX() + 10 * (kIcon + 10.0f));
        ui::TextDisabled("Right-click: half or one. Crouch-click: move across.");
        ui::PopTextWrapPos();
        // Armour: a slot for each piece (only that piece goes in it), and what they take off.
        ui::SeparatorText("Armour");
        for (int s = 0; s < static_cast<int>(BodySlot::Count); s++) {
            if (s > 0) ui::SameLine();
            DrawSlot(SlotBox::Armour, s, kIcon);
        }
        ui::SameLine();
        ui::AlignTextToFramePadding();
        ui::TextDisabled("%d%% less damage from hits and traps", static_cast<int>(std::lround(Armour::Protection(client->GetArmour()) * 100.0f)));
        DrawPlayerSlots(kIcon);
        // Offcuts: part blocks left from shaped blocks and recipes, used before whole ones.
        std::string cuts;
        const Inventory& inv = client->GetInventory();
        for (size_t t = 0; t < inv.offcuts.size(); t++)
            if (inv.offcuts[t]) cuts += (cuts.empty() ? "" : ", ") + FormatBlocks(inv.offcuts[t]) + " " + ItemName(Items::OfKind(t));
        if (!cuts.empty()) {
            ui::PushTextWrapPos(ui::GetCursorPosX() + 10 * (kIcon + 10.0f));
            ui::TextDisabled("Offcuts: %s", cuts.c_str());
            ui::PopTextWrapPos();
        }

        if (survival) {
            // Crafting: what you can make now from what you carry (and the tool in hand),
            // and buttons to make it (into your slots). (Everything else: the item panel.)
            ui::SeparatorText("Crafting");
            const auto& recipes = Crafting::Recipes();
            const Tool inHand = SelectedTool();
            // In a scrolling box, so the window stays on screen.
            ui::BeginChild("crafting", ui::Vec2(10 * (kIcon + 10.0f), 170.0f), true);
            int shown = 0;
            if (ui::BeginTable("recipes", 3, ui::TableFlags_RowBg | ui::TableFlags_SizingFixedFit)) {
                for (int r = 0; r < static_cast<int>(recipes.size()); r++) {
                    const Recipe& recipe = recipes[r];
                    if (!client->CanCraft(r, 1, inHand)) continue;
                    shown++;
                    ui::PushID(r);
                    ui::TableNextRow();
                    ui::TableNextColumn();
                    glm::vec2 uv0, uv1;
                    const int icon = recipe.ironUpgrade                    ? atlas.GetIndex("iron_ingot")
                                     : recipe.steelUpgrade                   ? atlas.GetIndex("steel_ingot")
                                     : recipe.tool != Tool::None             ? ToolTile(atlas, recipe.tool)
                                     : recipe.armour != ArmourPiece::Count ? atlas.GetIndex(Armour::Texture(recipe.armour))
                                                                           : ItemTile(atlas, recipe.output);
                    atlas.GetTileUV(icon, uv0, uv1);
                    ui::Image(texture, ui::Vec2(24.0f, 24.0f), ui::Vec2(uv0.x, uv0.y), ui::Vec2(uv1.x, uv1.y));
                    ui::SameLine();
                    ui::AlignTextToFramePadding();
                    ui::TextUnformatted(recipe.name.c_str());
                    ui::TableNextColumn();
                    std::string needs;
                    for (const auto& in : recipe.inputs)
                        needs += (needs.empty() ? "" : " + ") + FormatBlocks(in.units) + " " + ItemName(in.item);
                    if (recipe.Upgrade()) needs += recipe.steelUpgrade ? ", an iron tool in hand" : ", a stone tool in hand";
                    const bool can = client->CanCraft(r, 1, inHand);
                    if (can) ui::TextUnformatted(needs.c_str());
                    else ui::TextDisabled("%s", needs.c_str());
                    ui::TableNextColumn();
                    ui::BeginDisabled(!can);
                    if (ui::SmallButton("Make")) {
                        client->Craft(r, 1, inHand);
                        audio->PlayHere("craft", 0.6f);
                    }
                    ui::EndDisabled();
                    if (!recipe.Upgrade()) {
                        ui::SameLine();
                        ui::BeginDisabled(!client->CanCraft(r, 4));
                        if (ui::SmallButton("x4")) {
                            client->Craft(r, 4);
                            audio->PlayHere("craft", 0.6f);
                        }
                        ui::EndDisabled();
                    }
                    ui::PopID();
                }
                ui::EndTable();
            }
            if (shown == 0) ui::TextDisabled("Nothing you can make with what you carry (the item panel shows what everything takes).");
            ui::EndChild();
        }
    }
    ui::End();
    DrawItemPanel();
    HandleSlotClicks();
    DrawHeldStack();
    if (!open) SetInventoryOpen(false);
}

void Application::DrawItemPanel() {
    if (allItems.empty()) {
        for (ItemId item : ItemPalette::kItems) allItems.push_back(item);
        for (Tool t : ItemPalette::kTools)
            for (int tier = 0; tier < Items::kTiers; tier++) allItems.push_back(Items::OfTool(t, tier));
        for (int p = 0; p < static_cast<int>(ArmourPiece::Count); p++) allItems.push_back(Items::OfArmour(static_cast<ArmourPiece>(p)));
    }
    // What matches the search (by name, any case).
    auto lower = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string search = lower(itemSearch);
    paletteItems.clear();
    for (ItemId item : allItems)
        if (search.empty() || lower(ItemName(item)).find(search) != std::string::npos) paletteItems.push_back(item);

    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x - 10.0f, viewport->WorkPos.y + 200.0f), ui::Cond_Always, ui::Vec2(1.0f, 0.0f)); // (under the minimap)
    ui::SetNextWindowSize(ui::Vec2(kItemPanelWidth, std::max(260.0f, viewport->WorkSize.y - 330.0f)), ui::Cond_Always);
    ui::SetNextWindowBgAlpha(0.85f);
    const int flags = ui::WindowFlags_NoCollapse | ui::WindowFlags_NoMove | ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoResize;
    if (ui::Begin("Items", nullptr, flags)) {
        const bool creative = client->IsCreative();
        ui::SetNextItemWidth(-1.0f);
        ui::InputTextWithHint("##search", "Search", itemSearch, sizeof itemSearch);
        ui::TextDisabled(creative ? "Take a stack; put one here to get rid of it." : "Click one to see how to get it.");
        constexpr float kSlot = 30.0f;
        const int columns = std::max(1, static_cast<int>((kItemPanelWidth - 40.0f) / (kSlot + 6.0f + ui::GetStyle().ItemSpacing.x)));
        const float infoHeight = itemInfo != Items::kNone ? 170.0f : 0.0f;
        ui::BeginChild("items", ui::Vec2(0.0f, -infoHeight), true);
        DrawSlotGrid(SlotBox::Palette, 0, static_cast<int>(paletteItems.size()), columns, kSlot);
        if (paletteItems.empty()) ui::TextDisabled("Nothing called that.");
        ui::EndChild();
        if (itemInfo != Items::kNone) {
            ui::BeginChild("info", ui::Vec2(0.0f, 0.0f), false);
            DrawItemInfo(itemInfo);
            ui::EndChild();
        }
    }
    ui::End();
}

void Application::DrawItemInfo(ItemId item) {
    ui::PushTextWrapPos(ui::GetCursorPosX() + kItemPanelWidth - 30.0f);
    ui::SeparatorText(ItemName(item).c_str());
    if (Items::IsTool(item)) ui::TextUnformatted(ToolHint(Items::ToolOf(item)));
    if (Items::IsArmour(item)) ui::Text("%d%% less damage", static_cast<int>(std::lround(Armour::Protection(Items::ArmourOf(item)) * 100.0f)));
    if (Items::IsBlock(item)) {
        const BlockType type = Items::BlockOf(item);
        const float hardness = Mining::Hardness(type);
        const Tool best = Mining::BestTool(type);
        if (hardness <= 0.0f) ui::TextDisabled("Breaks at once.");
        else ui::TextDisabled("Hardness %g%s%s.", hardness, best != Tool::None ? ", faster with a " : "", best != Tool::None ? Mining::ToolName(best) : "");
    }
    auto inputs = [](const std::vector<Ingredient>& ins) {
        std::string s;
        for (const auto& in : ins) s += (s.empty() ? "" : " + ") + FormatBlocks(in.units) + " " + ItemName(in.item);
        return s;
    };
    // How it's made: the recipes that make it, and what smelts into it.
    const auto& recipes = Crafting::Recipes();
    const Tool inHand = SelectedTool();
    bool any = false;
    for (int r = 0; r < static_cast<int>(recipes.size()); r++) {
        const Recipe& recipe = recipes[r];
        bool makes = false;
        if (recipe.tool != Tool::None) makes = Items::IsTool(item) && Items::ToolOf(item) == recipe.tool && Items::TierOf(item) == (recipe.steelUpgrade ? 2 : recipe.ironUpgrade ? 1 : 0);
        else if (recipe.armour != ArmourPiece::Count) makes = Items::ArmourOf(item) == recipe.armour;
        else makes = !recipe.Upgrade() && static_cast<ItemId>(recipe.output) == item;
        if (!makes) continue;
        if (!any) ui::TextUnformatted("Made from:");
        any = true;
        std::string line = "  " + inputs(recipe.inputs);
        if (recipe.Upgrade()) line += recipe.steelUpgrade ? ", an iron one in hand" : ", a stone one in hand";
        if (recipe.outputUnits > Block::kUnitsPerBlock) line += " (makes " + FormatBlocks(recipe.outputUnits) + ")";
        if (!client->IsCreative() && client->CanCraft(r, 1, inHand)) ui::TextColored(ui::Vec4(0.6f, 1.0f, 0.6f, 1.0f), "%s (you can make it)", line.c_str());
        else ui::TextUnformatted(line.c_str());
    }
    for (const auto& s : Smelting::Recipes()) {
        if (static_cast<ItemId>(s.output) != item) continue;
        if (!any) ui::TextUnformatted("Made from:");
        any = true;
        std::string line = "  " + FormatBlocks(s.inputUnits) + " " + ItemName(s.input);
        if (static_cast<ItemId>(s.extra) != Items::kNone) line += " + " + FormatBlocks(s.extraUnits) + " " + ItemName(s.extra);
        if (s.outputUnits > Block::kUnitsPerBlock) line += " (makes " + FormatBlocks(s.outputUnits) + ")";
        ui::Text("%s, in a smelter", line.c_str());
    }
    if (!any && Items::IsBlock(item)) ui::TextDisabled("Found in the world (it can't be made).");
    else if (!any) ui::TextDisabled("Found, traded or dropped (it can't be made).");
    // What it's for: the recipes it goes into.
    std::string uses;
    for (const auto& recipe : recipes)
        for (const auto& in : recipe.inputs)
            if (static_cast<ItemId>(in.item) == item) {
                uses += (uses.empty() ? "" : ", ") + recipe.name;
                break;
            }
    for (const auto& s : Smelting::Recipes())
        if (static_cast<ItemId>(s.input) == item || static_cast<ItemId>(s.extra) == item) uses += (uses.empty() ? "" : ", ") + ItemName(s.output) + " (smelted)";
    if (!uses.empty()) {
        ui::TextUnformatted("Goes into:");
        ui::TextDisabled("  %s", uses.c_str());
    }
    ui::PopTextWrapPos();
}

Stack Application::StackIn(SlotBox box, int slot) const {
    if (slot < 0) return {};
    switch (box) {
    case SlotBox::Player: {
        const auto& slots = client->GetInventory().slots;
        return slot < static_cast<int>(slots.size()) ? slots[slot] : Stack{};
    }
    case SlotBox::Armour:
        if (slot >= static_cast<int>(BodySlot::Count)) return {};
        if (const ArmourPiece p = Armour::InSlot(client->GetArmour(), static_cast<BodySlot>(slot)); p != ArmourPiece::Count) return { Items::OfArmour(p), 1 };
        return {};
    case SlotBox::Chest: {
        const auto& slots = client->GetChest().slots;
        return slot < static_cast<int>(slots.size()) ? slots[slot] : Stack{};
    }
    case SlotBox::Palette:
        if (slot >= static_cast<int>(paletteItems.size())) return {};
        return { paletteItems[slot], static_cast<uint8_t>(Items::MaxStack(paletteItems[slot])) };
    }
    return {};
}

void Application::DrawSlot(SlotBox box, int slot, float size, bool selected) {
    const Stack st = StackIn(box, slot);
    const TextureAtlas& atlas = renderer->GetAtlas();
    const ui::TextureId texture = static_cast<ui::TextureId>(static_cast<uintptr_t>(atlas.GetTexture()));
    constexpr float kPad = 3.0f;
    const ui::Vec2 lo = ui::GetCursorScreenPos();
    const ui::Vec2 hi(lo.x + size + 2 * kPad, lo.y + size + 2 * kPad);
    ui::PushID(static_cast<int>(box) * 4096 + slot);
    ui::InvisibleButton("slot", ui::Vec2(hi.x - lo.x, hi.y - lo.y));
    const bool hovered = ui::IsItemHovered();
    ui::PopID();
    if (hovered) hoveredSlot = std::make_pair(box, slot);
    ui::DrawList* draw = ui::GetWindowDrawList();
    draw->AddRectFilled(lo, hi, hovered ? UI_COL32(255, 255, 255, 45) : UI_COL32(0, 0, 0, 90), 3.0f);
    const bool lifted = heldStack && heldStack->box == box && heldStack->slot == slot && box != SlotBox::Palette;
    if (!st.Empty()) DrawStackOn(draw, atlas, texture, lo, hi, kPad, st, lifted, box != SlotBox::Palette);
    else if (box == SlotBox::Armour) {
        // An empty armour slot shows what goes there, faintly.
        glm::vec2 uv0, uv1;
        atlas.GetTileUV(atlas.GetIndex(Armour::Texture(Armour::SlotIcon(static_cast<BodySlot>(slot)))), uv0, uv1);
        draw->AddImage(texture, ui::Vec2(lo.x + kPad, lo.y + kPad), ui::Vec2(hi.x - kPad, hi.y - kPad), ui::Vec2(uv0.x, uv0.y), ui::Vec2(uv1.x, uv1.y), UI_COL32(255, 255, 255, 45));
    }
    draw->AddRect(lo, hi, selected ? UI_COL32(255, 255, 255, 255) : UI_COL32(255, 255, 255, 50), 3.0f, 0, selected ? 2.5f : 1.0f);
    if (hovered && !heldStack && (!st.Empty() || box == SlotBox::Armour)) {
        const ItemId item = st.Empty() ? Items::OfArmour(Armour::SlotIcon(static_cast<BodySlot>(slot))) : st.item;
        std::string tip = box == SlotBox::Armour && st.Empty() ? std::string(Armour::SlotName(static_cast<BodySlot>(slot))) + " (empty)" : ItemName(item);
        if (Items::IsTool(item)) tip += std::string(": ") + ToolHint(Items::ToolOf(item));
        if (Items::IsArmour(item))
            if (!st.Empty())
                tip += std::string(" (") + Armour::SlotName(Armour::SlotOf(Items::ArmourOf(item))) + "): " +
                       std::to_string(static_cast<int>(std::lround(Armour::Protection(Items::ArmourOf(item)) * 100.0f))) + "% less damage" +
                       (Items::ArmourOf(item) == ArmourPiece::OxygenTank ? " (holds air to breathe instead)" : "") +
                       (Items::ArmourOf(item) == ArmourPiece::Mask ? ", keeps steam out" : "");
        if (Items::IsStackable(item) && box != SlotBox::Palette && st.count > 1) tip += " x" + std::to_string(st.count);
        ui::SetTooltip("%s", tip.c_str());
    }
}

void Application::DrawSlotGrid(SlotBox box, int first, int count, int columns, float size) {
    for (int i = 0; i < count; i++) {
        if (i % columns != 0) ui::SameLine();
        DrawSlot(box, first + i, size, box == SlotBox::Player && first + i == selectedSlot);
    }
}

void Application::DrawPlayerSlots(float size) {
    const int total = static_cast<int>(client->GetInventory().slots.size());
    ui::SeparatorText("Inventory");
    DrawSlotGrid(SlotBox::Player, kHotbarSlots, total - kHotbarSlots, kHotbarSlots, size);
    ui::SeparatorText("Hotbar");
    DrawSlotGrid(SlotBox::Player, 0, std::min(total, kHotbarSlots), kHotbarSlots, size);
}

void Application::HandleSlotClicks() {
    // The item panel: a click picks the item whose details it shows; in survival that's
    // all it does (nothing's taken from it).
    if (hoveredSlot && hoveredSlot->first == SlotBox::Palette && !heldStack &&
        (ui::IsMouseClicked(ui::MouseButton_Left) || ui::IsMouseClicked(ui::MouseButton_Right))) {
        if (const Stack st = StackIn(SlotBox::Palette, hoveredSlot->second); !st.Empty()) itemInfo = st.item;
        if (!client->IsCreative()) return;
    }
    // A stack picked up that's gone from its slot (moved, used up): let go of it.
    if (heldStack && heldStack->box != SlotBox::Palette) {
        const Stack st = StackIn(heldStack->box, heldStack->slot);
        if (st.Empty() || st.item != heldStack->item) heldStack.reset(), draggingStack = false;
        else heldStack->count = std::min(heldStack->count, static_cast<int>(st.count));
    }
    auto isHeld = [&](const std::pair<SlotBox, int>& h) { return heldStack && heldStack->box == h.first && heldStack->slot == h.second; };
    // Outside every window: what's held is thrown out onto the ground.
    const bool outside = !ui::IsWindowHovered(ui::HoveredFlags_AnyWindow) && !ui::IsAnyItemHovered();
    auto throwHeld = [&] {
        if (heldStack->box != SlotBox::Palette) {
            const net::Box box = heldStack->box == SlotBox::Armour ? net::Box::Armour : heldStack->box == SlotBox::Chest ? net::Box::Chest : net::Box::Player;
            const Stack st = StackIn(heldStack->box, heldStack->slot);
            client->DropItem(box, heldStack->slot, heldStack->count >= st.count ? 0 : heldStack->count, camera.GetDirection());
            audio->PlayHere("pickup", 0.25f);
        }
        heldStack.reset();
        draggingStack = false;
    };
    if (ui::IsMouseClicked(ui::MouseButton_Left)) {
        if (!hoveredSlot) {
            if (heldStack && outside) throwHeld();
            else if (heldStack && !ui::IsAnyItemHovered()) heldStack.reset(); // (clicked off the slots: let go)
        }
        else if (heldStack) {
            if (!isHeld(*hoveredSlot)) MoveStack(*heldStack, hoveredSlot->first, hoveredSlot->second, heldStack->count);
            heldStack.reset();
        }
        else if (const Stack st = StackIn(hoveredSlot->first, hoveredSlot->second); !st.Empty()) {
            if (CrouchHeld()) QuickMove(hoveredSlot->first, hoveredSlot->second);
            else {
                heldStack = HeldStack{ hoveredSlot->first, hoveredSlot->second, st.item, st.count };
                draggingStack = true;
            }
        }
    }
    // Let go of a dragged stack over another slot: it goes there. (Over its own slot, it
    // stays picked up, to be put down with a click.)
    if (ui::IsMouseReleased(ui::MouseButton_Left) && draggingStack) {
        draggingStack = false;
        if (heldStack && !hoveredSlot && outside) throwHeld();
        else if (heldStack && hoveredSlot && !isHeld(*hoveredSlot)) {
            MoveStack(*heldStack, hoveredSlot->first, hoveredSlot->second, heldStack->count);
            heldStack.reset();
        }
    }
    if (ui::IsMouseClicked(ui::MouseButton_Right) && hoveredSlot) {
        if (!heldStack) {
            if (const Stack st = StackIn(hoveredSlot->first, hoveredSlot->second); !st.Empty()) {
                const int half = hoveredSlot->first == SlotBox::Palette ? 1 : (st.count + 1) / 2;
                heldStack = HeldStack{ hoveredSlot->first, hoveredSlot->second, st.item, half };
                draggingStack = false;
            }
        }
        else if (!isHeld(*hoveredSlot)) {
            MoveStack(*heldStack, hoveredSlot->first, hoveredSlot->second, 1);
            if (heldStack->box != SlotBox::Palette && --heldStack->count <= 0) heldStack.reset();
        }
    }
}

void Application::DrawHeldStack() {
    if (!heldStack) return;
    const TextureAtlas& atlas = renderer->GetAtlas();
    const ui::TextureId texture = static_cast<ui::TextureId>(static_cast<uintptr_t>(atlas.GetTexture()));
    const ui::Vec2 mouse = ui::GetIO().MousePos;
    const ui::Vec2 lo(mouse.x - 18.0f, mouse.y - 18.0f), hi(mouse.x + 18.0f, mouse.y + 18.0f);
    DrawStackOn(ui::GetForegroundDrawList(), atlas, texture, lo, hi, 0.0f, { heldStack->item, static_cast<uint8_t>(std::clamp(heldStack->count, 1, 255)) }, false, true);
}

void Application::QuickMove(SlotBox box, int slot) {
    const Stack st = StackIn(box, slot);
    if (st.Empty()) return;
    switch (box) {
    case SlotBox::Player:
        // Into the open chest; armour onto you; else between the hotbar and the rest.
        if (chestMode && client->GetOpenChest()) client->MoveItems(net::Box::Player, slot, net::Box::Chest, net::kAnySlot);
        else if (Items::IsArmour(st.item)) client->MoveItems(net::Box::Player, slot, net::Box::Armour, net::kAnySlot);
        else client->MoveItems(net::Box::Player, slot, net::Box::Player, net::kAnySlot);
        break;
    case SlotBox::Armour: client->MoveItems(net::Box::Armour, slot, net::Box::Player, net::kAnySlot); break;
    case SlotBox::Chest: client->MoveItems(net::Box::Chest, slot, net::Box::Player, net::kAnySlot); break;
    case SlotBox::Palette: client->SetCreativeSlot(selectedSlot, st.item, st.count); break; // (into the hotbar slot in hand)
    }
    audio->PlayHere("pickup", 0.3f);
}

void Application::MoveStack(const HeldStack& from, SlotBox box, int slot, int count) {
    auto netBox = [](SlotBox b) { return b == SlotBox::Armour ? net::Box::Armour : b == SlotBox::Chest ? net::Box::Chest : net::Box::Player; };
    if (from.box == SlotBox::Palette) {
        // Creative: a stack of it (onto the same, more of it).
        if (box != SlotBox::Player) return;
        const Stack there = StackIn(box, slot);
        client->SetCreativeSlot(slot, from.item, there.item == from.item ? there.count + count : count);
        return;
    }
    if (box == SlotBox::Palette) {
        // Put back: gone.
        if (from.box == SlotBox::Player && client->IsCreative()) client->SetCreativeSlot(from.slot, Items::kNone, 0);
        return;
    }
    const Stack st = StackIn(from.box, from.slot);
    client->MoveItems(netBox(from.box), from.slot, netBox(box), slot, count >= st.count ? 0 : count);
    audio->PlayHere("pickup", 0.3f);
}

std::optional<glm::ivec3> Application::StructureCoreAt(const glm::ivec3& pos) const {
    // Structures are 3 across and a few high: a core within reach whose pattern covers it.
    for (int dx = -2; dx <= 2; dx++)
        for (int dz = -2; dz <= 2; dz++)
            for (int dy = -5; dy <= 5; dy++) {
                const glm::ivec3 p = pos + glm::ivec3(dx, dy, dz);
                Block core;
                if (!world->GetBlock(p.x, p.y, p.z, core) || core.type != BlockType::MULTIBLOCK) continue;
                const glm::ivec3 origin = Multiblocks::OriginOf(p, core);
                for (const auto& cell : Multiblocks::PatternOf(Multiblocks::KindOf(core)).cells)
                    if (Multiblocks::Place(origin, Multiblocks::TurnOf(core), cell.at) == pos) return p;
            }
    return std::nullopt;
}

void Application::OpenMachine(const glm::ivec3& pos, bool inventory) {
    machineAt = pos;
    machineAskedAt = machineInspectAt = Client::Now();
    Block b;
    if (world->GetBlock(pos.x, pos.y, pos.z, b) && Power::IsPart(b.type)) client->Inspect(pos);
    SetInventoryOpen(true);
    if (inventory) {
        client->OpenChest(pos);
        chestMode = true;
        chestSeenOpen = false;
        chestAskedAt = Client::Now();
    }
    else machineMode = true;
}

void Application::DrawMachineInfo(const glm::ivec3& pos) {
    Block b;
    if (!world->GetBlock(pos.x, pos.y, pos.z, b) || !Power::IsPart(b.type)) return;
    const double now = Client::Now();
    // The description is asked for again every second, so it stays current.
    if (now - machineInspectAt > 1.0) {
        client->Inspect(pos);
        machineInspectAt = now;
    }
    ui::PushTextWrapPos(ui::GetCursorPosX() + 460.0f);
    const auto& said = client->GetInspections();
    if (!said.empty() && said.back().time >= machineAskedAt - 0.5) {
        ui::PushStyleColor(ui::Col_Text, ui::Vec4(0.8f, 0.8f, 0.8f, 1.0f));
        ui::TextUnformatted(said.back().text.c_str());
        ui::PopStyleColor();
    }
    else ui::TextDisabled("...");
    ui::PopTextWrapPos();
    ui::Separator();
}

void Application::DrawMachine() {
    // Gone, or walked away from: the menu goes.
    Block b;
    if (!world->GetBlock(machineAt.x, machineAt.y, machineAt.z, b) || !Power::IsPart(b.type) ||
        glm::distance(camera.GetPosition(), glm::vec3(machineAt) + 0.5f) > 8.0f) {
        SetInventoryOpen(false);
        return;
    }
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ui::Cond_Always, ui::Vec2(0.5f, 0.5f));
    ui::SetNextWindowBgAlpha(0.85f);
    const int flags = ui::WindowFlags_NoCollapse | ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove |
                                   ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoResize;
    bool keepOpen = true;
    const std::string title = std::string(BlockName(b.type)) + "###machine";
    if (ui::Begin(title.c_str(), &keepOpen, flags)) {
        DrawMachineInfo(machineAt);
    }
    ui::End();
    if (!keepOpen) SetInventoryOpen(false);
}

void Application::DrawChest() {
    // Waiting for the server to say it's open; once open, closed by the server (broken,
    // walked away from): the window goes.
    const bool open = client->GetOpenChest().has_value();
    if (open) chestSeenOpen = true;
    if ((chestSeenOpen && !open) || (!chestSeenOpen && Client::Now() - chestAskedAt > 3.0)) {
        SetInventoryOpen(false);
        return;
    }
    constexpr float kIcon = 36.0f;
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f - kItemPanelWidth * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ui::Cond_Always, ui::Vec2(0.5f, 0.5f));
    ui::SetNextWindowBgAlpha(0.85f);
    const int flags = ui::WindowFlags_NoCollapse | ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove |
                                   ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoResize;
    bool keepOpen = true;
    std::string title = "Chest";
    if (const auto at = client->GetOpenChest()) {
        Block b;
        if (world->GetBlock(at->x, at->y, at->z, b) && b.type != BlockType::CHEST)
            title = b.type == BlockType::MULTIBLOCK ? Multiblocks::PatternOf(Multiblocks::KindOf(b)).name : BlockName(b.type);
    }
    title += "###chest";
    hoveredSlot.reset();
    if (ui::Begin(title.c_str(), &keepOpen, flags)) {
        if (const auto at = client->GetOpenChest()) DrawMachineInfo(*at); // (a machine: how it's doing)
        ui::TextDisabled("Right-click: half or one. Crouch-click: move across.");
        ui::SeparatorText("In it");
        DrawSlotGrid(SlotBox::Chest, 0, static_cast<int>(client->GetChest().slots.size()), 10, kIcon);
        std::string cuts;
        const Inventory& chest = client->GetChest();
        for (size_t t = 0; t < chest.offcuts.size(); t++)
            if (chest.offcuts[t]) cuts += (cuts.empty() ? "" : ", ") + FormatBlocks(chest.offcuts[t]) + " " + ItemName(Items::OfKind(t));
        if (!cuts.empty()) ui::TextDisabled("Offcuts: %s", cuts.c_str());
        DrawPlayerSlots(kIcon);
    }
    ui::End();
    DrawItemPanel();
    HandleSlotClicks();
    DrawHeldStack();
    if (!keepOpen) SetInventoryOpen(false);
}

std::optional<Application::ChiselAim> Application::AimChisel() {
    // Only with the chisel in hand (made, in survival), at something it can shape.
    if (SelectedTool() != Tool::Chisel || !client->HasTool(Tool::Chisel)) return std::nullopt;
    const glm::vec3 dir = camera.GetDirection();
    auto hit = Raycast(*world, camera.GetPosition(), dir, Reach());
    if (!hit) return std::nullopt;
    ChiselAim aim;
    aim.block = hit->block;
    if (!world->GetBlock(hit->block.x, hit->block.y, hit->block.z, aim.target) || !aim.target.CanChisel()) return std::nullopt;
    // A full block is chiselled from the side looked at (that side becomes its top); a
    // shaped one in its own direction. The quarter is the one the view meets: where it
    // touches the shape (a hair on into it, so a quarter's side counts as that quarter),
    // across the shape's own upright frame.
    aim.shapeDir = aim.target.IsFullBlock() && Block::CanTurnShape(aim.target.type) ? Block::ShapeDirFacing(hit->normal) : aim.target.ShapeDir();
    const glm::vec3 p = glm::clamp(hit->point + dir * 0.002f - glm::vec3(hit->block), glm::vec3(0.0f), glm::vec3(1.0f));
    aim.edge = Block::ChiselEdgeAt(aim.shapeDir, p);
    return aim;
}

float Application::ThrowStrength() const {
    if (!throwHeld) return 0.0f;
    const double held = glfwGetTime() - throwSince;
    return held < kThrowTap ? 0.0f : static_cast<float>(std::min(1.0, (held - kThrowTap) / (kThrowWindUp - kThrowTap)));
}

void Application::UpdateThrow() {
    if (!throwHeld) return;
    // (Opening a menu or the chat puts it down unthrown.)
    if (!captureCursor || chatOpen || inventoryOpen) {
        throwHeld = false;
        return;
    }
    if (glfwGetKey(window, settings.Key(Action::Drop)) == GLFW_PRESS) return;
    // Let go: the item in hand, one of it (crouching: the whole stack), as hard as it was wound up.
    const float strength = ThrowStrength();
    throwHeld = false;
    const auto& slots = client->GetInventory().slots;
    if (selectedSlot < 0 || selectedSlot >= static_cast<int>(slots.size()) || slots[selectedSlot].Empty()) return;
    client->DropItem(net::Box::Player, selectedSlot, CrouchHeld() ? 0 : 1, camera.GetDirection(), strength);
    audio->PlayHere("pickup", 0.25f + 0.3f * strength);
}

bool Application::CrouchHeld() const {
    if (glfwGetKey(window, settings.Key(Action::Crouch)) == GLFW_PRESS) return true;
    GLFWgamepadstate pad;
    return glfwGetGamepadState(GLFW_JOYSTICK_1, &pad) && pad.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS;
}

void Application::ChiselTarget(bool raise) {
    if (const auto aim = AimChisel()) client->Chisel(aim->block, aim->edge, raise, aim->shapeDir, chiselStep);
}

void Application::OnMouseButton(int button, int action) {
    // With the cursor released, clicks belong to the UI.
    if (action != GLFW_PRESS || !captureCursor || chatOpen) return;

    // A creature in the way: hit it (with whatever's in hand).
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        const FightTarget target = TargetInView();
        if (target.creature) {
            client->Attack(target.creature, client->Usable(SelectedTool()));
            return;
        }
        if (target.player >= 0) {
            client->AttackPlayer(static_cast<uint16_t>(target.player), client->Usable(SelectedTool()));
            return;
        }
    }

    // Middle-click picks the block looked at into the hotbar slot (in survival, only
    // what you have some of).
    if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        Block looked;
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), Reach());
            at && world->GetBlock(at->block.x, at->block.y, at->block.z, looked)) {
            // (What it counts as, if that's another block: pebbles are stone, an open door a
            // door. Coal ore is coal ore, though breaking it gives coal.)
            const ItemId worth = Items::ResourceOf(looked.type);
            const BlockType type = Items::IsBlock(worth) ? Items::BlockOf(worth) : worth == Items::kNone ? BlockType::AIR : looked.type;
            const ItemId item = Items::OfBlock(type);
            if (type == BlockType::AIR) {} // (part of a structure: nothing to pick)
            else if (client->IsCreative()) client->SetCreativeSlot(selectedSlot, item, static_cast<int>(Items::MaxStack(item)));
            else {
                // Survival: a hotbar slot holding it is chosen; else a stack of it from the
                // rest of the inventory swaps into the slot in hand.
                const auto& slots = client->GetInventory().slots;
                bool found = false;
                for (int i = 0; i < kHotbarSlots && i < static_cast<int>(slots.size()) && !found; i++)
                    if (!slots[i].Empty() && slots[i].item == item) SelectSlot(i), found = true;
                for (int i = kHotbarSlots; i < static_cast<int>(slots.size()) && !found; i++)
                    if (!slots[i].Empty() && slots[i].item == item) client->MoveItems(net::Box::Player, i, net::Box::Player, selectedSlot), found = true;
            }
        }
        return;
    }

    // Right-click on a creature: feed it (raw venison tames a wild wolf).
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        const FightTarget target = TargetInView();
        if (target.creature) {
            // A trader: its offers. (Anything else: fed, or tamed.)
            for (const auto& c : client->GetCreatures())
                if (c.id == target.creature && c.type == CreatureType::DwarfTrader) {
                    SetInventoryOpen(true);
                    tradeMode = true;
                    tradeWith = c.id;
                    client->tradePrices.clear();
                    client->Interact(c.id); // (the server answers with its prices)
                    return;
                }
            client->Interact(target.creature);
            return;
        }
    }

    // Right-click on a chest within 6 blocks opens it (crouching, or further off, it's
    // built against instead; but crouching at a signal block sets its channels).
    Block aimedAt;
    const auto aimedHit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
    const bool atSignalBlock = aimedHit && world->GetBlock(aimedHit->block.x, aimedHit->block.y, aimedHit->block.z, aimedAt) &&
                               Signals::KindOf(aimedAt.type) != Signals::Kind::None;
    // (Crouching at a door with no block in hand: it swaps which side it's hinged.)
    if (button == GLFW_MOUSE_BUTTON_RIGHT && CrouchHeld() && aimedHit && (aimedAt.type == BlockType::DOOR || aimedAt.type == BlockType::DOOR_OPEN) &&
        (SelectedTool() != Tool::None || SelectedBlock() == BlockType::AIR)) {
        client->UseBed(aimedHit->block);
        return;
    }
    if (button == GLFW_MOUSE_BUTTON_RIGHT && (!CrouchHeld() || atSignalBlock)) {
        Block target;
        // The engineer's hammer: strike a structure's block to form it (the server
        // checks the pattern and says what's missing).
        if (SelectedItemType() == ItemType::HAMMER) {
            if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f)) client->UseBed(at->block);
            return;
        }
        // Menus come before building: a machine with its own inventory opens like a chest,
        // with how it's doing at the top, whatever's in hand (crouch: build against it
        // instead). Any block of a formed structure (its ports too) opens the
        // structure's. A crafter takes what's in hand as its recipe.
        // (Holding machinery that joins machines (a pipe, a gauge, a shaft, a wheel, a
        // valve), right-clicking a machine builds it on, as that's how they're connected.)
        const bool holding = SelectedTool() == Tool::None && SelectedBlock() != BlockType::AIR;
        const BlockType inHand = SelectedBlock();
        const bool connecting = holding && (IsPipeType(inHand) || inHand == BlockType::PRESSURE_GAUGE || inHand == BlockType::SHAFT ||
                                            inHand == BlockType::WATER_WHEEL || IsValveType(inHand));
        auto opens = [&](BlockType t) {
            if (t == BlockType::CHEST || t == BlockType::MINING_POST || t == BlockType::GRAVE) return true;
            if (t == BlockType::CRAFTER) return !holding;
            return IsMachineInventory(t) && !connecting;
        };
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
            at && world->GetBlock(at->block.x, at->block.y, at->block.z, target)) {
            glm::ivec3 pos = at->block;
            if (!connecting)
                if (const auto core = StructureCoreAt(pos); core && world->GetBlock(core->x, core->y, core->z, target)) pos = *core;
            if (opens(target.type)) return OpenMachine(pos, true);
        }
        // A filter pipe or a crafter: set it to what's in hand (crouching, it's
        // built against instead).
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
            at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && (target.type == BlockType::FILTER_PIPE || target.type == BlockType::CRAFTER)) {
            client->UseBed(at->block);
            return;
        }
        // With a block in hand, right-clicking a pipe, a shaft, a wheel or a valve builds
        // against it (a pipe run on, a wheel on a shaft); anything with a menu opens it.
        const bool building = SelectedBlock() != BlockType::AIR;
        auto buildsOn = [&](BlockType t) {
            return building && (IsPipeType(t) || t == BlockType::SHAFT || t == BlockType::WATER_WHEEL || IsValveType(t));
        };
        // A signal block: crouching, set its channels; a gate or a lamp (nothing else to
        // do with them), likewise.
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
            at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && Signals::KindOf(target.type) != Signals::Kind::None &&
            (CrouchHeld() ||
             (!connecting && (target.type == BlockType::LOGIC_GATE || target.type == BlockType::SIGNAL_LAMP || target.type == BlockType::SIGNAL_LAMP_ON)))) {
            OpenSignalEditor(at->block, target.type);
            return;
        }
        // A sign: write on it.
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
            at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && target.type == BlockType::SIGN) {
            OpenSign(at->block);
            return;
        }
        // A bed: respawn here (and sleep, at night).
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
            at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) &&
            (target.type == BlockType::BED || target.type == BlockType::DOOR || target.type == BlockType::DOOR_OPEN || target.type == BlockType::CAMPFIRE ||
             target.type == BlockType::TNT || target.type == BlockType::LEVER || target.type == BlockType::LEVER_ON ||
             (!building && IsValveType(target.type)))) {
            client->UseBed(at->block);
            return;
        }
        // Any other machine, or a pipe: its menu, saying how it's doing.
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
            at && !connecting && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && Power::IsPart(target.type) && !buildsOn(target.type))
            return OpenMachine(at->block, false);
    }

    // Right-click with the shovel: level the ground round the block looked at to its height.
    if (button == GLFW_MOUSE_BUTTON_RIGHT && SelectedTool() == Tool::Shovel) {
        if (auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), Reach())) {
            client->Flatten(hit->block, flattenRadius);
            PlayAt("place", hit->block, 0.6f);
        }
        return;
    }
    // Right-click with the chisel: cut the corner looked at (crouching: build it up).
    if (button == GLFW_MOUSE_BUTTON_RIGHT && SelectedTool() == Tool::Chisel) {
        ChiselTarget(CrouchHeld());
        return;
    }

    // Right-click with the rod: cast into the water looked at (the first water surface
    // along the view, within 12 blocks), or reel in.
    if (button == GLFW_MOUSE_BUTTON_RIGHT && SelectedTool() == Tool::Rod) {
        if (client->GetBobber()) {
            client->Cast(*client->GetBobber(), false);
            return;
        }
        if (!client->HasTool(Tool::Rod)) return;
        const glm::vec3 eye = camera.GetPosition(), dir = camera.GetDirection();
        for (float t = 0.5f; t < 12.0f; t += 0.1f) {
            const glm::ivec3 cell(glm::floor(eye + dir * t));
            Block b, above;
            if (!world->GetBlock(cell.x, cell.y, cell.z, b)) break;
            if (b.HasWater() && !(world->GetBlock(cell.x, cell.y + 1, cell.z, above) && above.HasWater())) {
                client->Cast(cell, true);
                audio->Play("splash", glm::vec3(cell) + glm::vec3(0.5f), 0.4f, 0.2f);
                break;
            }
            if (!b.IsPassable()) break; // (hit something solid first)
        }
        return;
    }

    // Right-click with a crossbow in hand: a bolt (slower to wind than a bow).
    if (button == GLFW_MOUSE_BUTTON_RIGHT && SelectedItemType() == ItemType::CROSSBOW) {
        if ((client->IsCreative() || (client->GetResources(ItemType::CROSSBOW) > 0 && client->GetResources(ItemType::ARROW) >= Block::kUnitsPerBlock)) &&
            Client::Now() >= nextShot) {
            client->Shoot(camera.GetDirection());
            audio->PlayHere("bow", 0.9f);
            nextShot = Client::Now() + 1.2;
        }
        return;
    }
    // Right-click with the bow drawn: shoot (if there's an arrow, in survival).
    if (button == GLFW_MOUSE_BUTTON_RIGHT && SelectedTool() == Tool::Bow) {
        if (client->HasTool(Tool::Bow) && (client->IsCreative() || client->GetResources(ItemType::ARROW) >= Block::kUnitsPerBlock) &&
            Client::Now() >= nextShot) {
            client->Shoot(camera.GetDirection());
            audio->PlayHere("bow", 0.7f);
            nextShot = Client::Now() + 0.6;
        }
        return;
    }

    // Right-click with an item in hand uses it (wherever you look): food is eaten;
    // items are never placed.
    if (button == GLFW_MOUSE_BUTTON_RIGHT && Items::IsItem(SelectedItem())) {
        const ItemType item = SelectedItemType();
        // A bucket: scoop up the (full) water looked at; a water bucket: pour it out where
        // you're looking (on fire: it puts it out, and the fires round it).
        if (item == ItemType::BUCKET) {
            const glm::vec3 eye = camera.GetPosition(), dir = camera.GetDirection();
            for (float t = 0.3f; t < Reach(); t += 0.1f) {
                const glm::ivec3 cell(glm::floor(eye + dir * t));
                Block b;
                if (!world->GetBlock(cell.x, cell.y, cell.z, b)) break;
                if (b.IsFullWater()) {
                    client->UseBucket(cell, false);
                    audio->Play("splash", glm::vec3(cell) + glm::vec3(0.5f), 0.4f, 0.2f);
                    break;
                }
                if (!b.IsPassable()) break;
            }
            return;
        }
        if (item == ItemType::WATER_BUCKET) {
            if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), Reach())) {
                Block aimed;
                const bool into = world->GetBlock(at->block.x, at->block.y, at->block.z, aimed) && aimed.IsReplaceableByWater();
                const glm::ivec3 cell = into ? at->block : at->block + at->normal;
                client->UseBucket(cell, true);
                audio->Play("splash", glm::vec3(cell) + glm::vec3(0.5f), 0.7f, 0.15f);
            }
            return;
        }
        // A boat: into it, on the water looked at (within 3 blocks: a hop the server's
        // movement check allows).
        if (item == ItemType::BOAT) {
            const glm::vec3 eye = camera.GetPosition(), dir = camera.GetDirection();
            for (float t = 0.5f; t < 3.0f && !player.IsBoating(); t += 0.1f) {
                const glm::ivec3 cell(glm::floor(eye + dir * t));
                Block b, above;
                if (!world->GetBlock(cell.x, cell.y, cell.z, b)) break;
                if (b.HasWater() && !(world->GetBlock(cell.x, cell.y + 1, cell.z, above) && above.HasWater())) {
                    if (client->IsCreative() || client->GetResources(ItemType::BOAT) >= Block::kUnitsPerBlock) {
                        player.Teleport(glm::vec3(cell) + glm::vec3(0.5f, b.WaterShownLevel() / static_cast<float>(Block::kMaxWaterVolume) - 0.15f, 0.5f));
                        player.SetBoating(true);
                        audio->Play("splash", glm::vec3(cell) + glm::vec3(0.5f), 0.4f, 0.2f);
                    }
                    break;
                }
                if (!b.IsPassable()) break;
            }
            return;
        }
        // Seeds are planted on the dirt or grass you're looking at.
        // (Or in place of a tuft of grass growing on it.)
        if (item == ItemType::SEEDS) {
            Block soil, aimed;
            if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), Reach())) {
                const bool intoPlant = world->GetBlock(at->block.x, at->block.y, at->block.z, aimed) && aimed.IsReplaceablePlant();
                const glm::ivec3 plot = intoPlant ? at->block : at->block + at->normal;
                if ((intoPlant || at->normal == glm::ivec3(0, 1, 0)) && world->GetBlock(plot.x, plot.y - 1, plot.z, soil) &&
                    (soil.type == BlockType::DIRT || IsGrassType(soil.type)) && client->CanAfford(BlockType::CROP, 8)) {
                    if (client->PlaceBlock(plot, BlockType::CROP)) PlayAt("place", plot, 0.5f);
                }
            }
            return;
        }
        if (Hunger::IsFood(Items::Of(item)) && !client->IsCreative() && client->GetResources(item) >= Block::kUnitsPerBlock &&
            client->GetFood() < client->GetMaxFood()) {
            client->Eat(Items::Of(item));
            audio->PlayHere("eat", 0.8f);
        }
        return;
    }

    auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), Reach());
    if (!hit) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        // Creative breaks at once; survival mines while the button is held (UpdateMining).
        if (client->IsCreative()) {
            Block broken;
            if (world->GetBlock(hit->block.x, hit->block.y, hit->block.z, broken)) {
                PlayAt(BreakSound(broken.type), hit->block);
                if (!broken.IsLiquid()) SpawnParticles(hit->block, broken.type, 18);
            }
            client->BreakBlock(hit->block);
        }
    }
    else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (SelectedTool() != Tool::None) return; // tools don't place anything
        // Aimed at a tuft of grass: the block replaces it, rather than going beside it.
        Block aimed;
        const bool intoPlant = world->GetBlock(hit->block.x, hit->block.y, hit->block.z, aimed) && aimed.IsReplaceablePlant();
        const glm::ivec3 place = intoPlant ? hit->block : hit->block + hit->normal;
        // Don't build a solid block inside yourself when walking (water, sprites and
        // pressure plates are fine).
        const BlockType placeBlock = SelectedBlock();
        if (placeBlock == BlockType::AIR) return; // (an empty hand, or armour: nothing to place)
        const int height = Block::CanShape(placeBlock) ? placeHeight : 8;
        if (!client->CanAfford(placeBlock, height)) return;
        const Block placed = Block::Placed(placeBlock, height);
        if (!(player.IsFlying() && !player.IsFlyingSolid()) && placed.IsFullBlock()) {
            const Collision::AABB body = player.GetBounds();
            const glm::vec3 lo(place), hi = lo + glm::vec3(1.0f);
            if (glm::all(glm::lessThan(lo, body.max)) && glm::all(glm::greaterThan(hi, body.min))) return;
        }
        // Logs lie along the axis of the face they're placed against; a wheel's or a
        // shaft's axle lies the way you're facing.
        int faceAxis = hit->normal.x != 0 ? 0 : hit->normal.z != 0 ? 2 : 1;
        if (placeBlock == BlockType::WATER_WHEEL || placeBlock == BlockType::SHAFT) {
            const glm::vec3 look = camera.GetDirection();
            faceAxis = std::abs(look.x) >= std::abs(look.z) ? 0 : 2;
        }
        // A piston pushes the way you're looking (up, down or level): a Face. A gauge
        // faces out from what it's fixed to.
        if (IsFacingType(placeBlock)) {
            const glm::vec3 look = camera.GetDirection(), a = glm::abs(look);
            faceAxis = a.y >= a.x && a.y >= a.z ? (look.y > 0 ? 2 : 3) : a.x >= a.z ? (look.x > 0 ? 0 : 1) : (look.z > 0 ? 4 : 5);
        }
        if (placeBlock == BlockType::PRESSURE_GAUGE || placeBlock == BlockType::FLUID_OUTLET || IsLeverType(placeBlock)) // (out of the face it's put on)
            for (int f = 0; f < kFaceCount; f++)
                if (glm::ivec3(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]) == hit->normal) faceAxis = f;
        // A door faces whoever puts it up (level); a ladder faces out from the wall it's
        // put on, or, put on top of another ladder, the way that one does.
        if (IsWallFacingType(placeBlock)) {
            const glm::vec3 look = camera.GetDirection();
            faceAxis = std::abs(look.x) >= std::abs(look.z) ? (look.x > 0.0f ? 1 : 0) : (look.z > 0.0f ? 5 : 4);
            if (placeBlock == BlockType::LADDER) {
                if (hit->normal.y == 0)
                    for (int f = 0; f < kFaceCount; f++)
                        if (glm::ivec3(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]) == hit->normal) faceAxis = f;
                if (aimed.type == BlockType::LADDER && hit->normal.y != 0) faceAxis = aimed.axis;
            }
        }
        // A door's hinge: beside another door, on the far side from it (a double door);
        // else on the side of the doorway clicked nearer.
        if (placeBlock == BlockType::DOOR) {
            const glm::ivec3 f(kFaceOffsets[faceAxis][0], 0, kFaceOffsets[faceAxis][2]), right(f.z, 0, -f.x);
            auto doorAt = [&](const glm::ivec3& p, bool& mirrored) {
                Block d;
                if (!world->GetBlock(p.x, p.y, p.z, d) || (d.type != BlockType::DOOR && d.type != BlockType::DOOR_OPEN) || d.Facing() != f) return false;
                mirrored = d.DoorMirrored();
                return true;
            };
            bool otherMirrored = false, mirror;
            if (doorAt(place - right, otherMirrored) && !otherMirrored) mirror = true;      // one on its left, hinged left: this one on the right
            else if (doorAt(place + right, otherMirrored) && otherMirrored) mirror = false; // one on its right, hinged right
            else mirror = glm::dot(hit->point - (glm::vec3(place) + glm::vec3(0.5f)), glm::vec3(right)) > 0.0f;
            if (mirror) faceAxis |= Block::kDoorMirrored;
        }
        if (!client->PlaceBlock(place, placeBlock, height, faceAxis)) return; // (refused, as the server would: no sound, no editor)
        PlayAt("place", place, 0.8f);
        if (placeBlock == BlockType::SIGN) OpenSign(place); // and write on it
        // Signal blocks: set their channels straight away.
        const Signals::Kind placedKind = Signals::KindOf(placeBlock);
        if (placeBlock == BlockType::LEVER || placeBlock == BlockType::LOGIC_GATE || placeBlock == BlockType::SIGNAL_LAMP || placedKind == Signals::Kind::Piston || placedKind == Signals::Kind::Launcher || placedKind == Signals::Kind::Sprinkler ||
            Signals::IsSensor(placedKind))
            OpenSignalEditor(place, placeBlock);
    }
}

void Application::OpenSign(const glm::ivec3& pos) {
    signAt = pos;
    const auto& signs = client->GetSigns();
    auto it = signs.find(pos);
    std::snprintf(signText, sizeof signText, "%s", it != signs.end() ? it->second.c_str() : "");
    SetInventoryOpen(true);
    signMode = true;
}

void Application::OpenSignalEditor(const glm::ivec3& pos, BlockType type) {
    signalAt = pos;
    signalKind = static_cast<int>(Signals::KindOf(type));
    signalModeIndex = signalA = signalB = signalOut = 0;
    const auto& infos = client->GetSignals();
    if (auto it = infos.find(pos); it != infos.end() && it->second.kind == signalKind) {
        signalModeIndex = it->second.mode;
        signalA = it->second.a;
        signalB = it->second.b;
        signalOut = it->second.out;
    }
    SetInventoryOpen(true);
    signalMode = true;
}

void Application::DrawSignalEditor() {
    const auto kind = static_cast<Signals::Kind>(signalKind);
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.4f),
                            ui::Cond_Always, ui::Vec2(0.5f, 0.5f));
    ui::SetNextWindowBgAlpha(0.9f);
    const char* titles[] = { "Signal", "Lever", "Pressure plate", "Logic gate", "Signal lamp", "Valve", "Sluice gate", "Piston",
                             "Water sensor", "Pressure sensor", "Daylight sensor", "Player sensor", "Machine sensor", "Air launcher", "Heat sensor", "Sprinkler" };
    static_assert(UI_ARRAYSIZE(titles) == static_cast<int>(Signals::Kind::Count), "a signal kind has no title");
    bool open = true;
    if (ui::Begin(titles[std::clamp(signalKind, 0, static_cast<int>(Signals::Kind::Count) - 1)], &open,
                     ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove | ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoCollapse)) {
        auto channel = [](const char* label, int& value) {
            ui::SetNextItemWidth(160.0f);
            if (ui::InputInt(label, &value)) value = std::clamp(value, 0, 255);
        };
        ui::TextDisabled("Channels 1-255 (0: none)");
        if (kind == Signals::Kind::Gate) {
            const char* modes[] = { "AND (on while A and B are)", "OR (on while A or B is)", "XOR (on while just one of A, B is)", "NOT (on while A isn't)",
                                    "RELAY (sends A on again, further)", "CLOCK (on and off each second while A is on, or always)",
                                    "LATCH (A turns it on, and it stays on until B turns it off)" };
            ui::SetNextItemWidth(360.0f);
            ui::Combo("Does", &signalModeIndex, modes, UI_ARRAYSIZE(modes));
            channel("Input A", signalA);
            if (Signals::Inputs(static_cast<Signals::Mode>(signalModeIndex)) == 2) channel("Input B", signalB);
            channel("Sends on", signalOut);
        }
        else if (Signals::Sends(kind)) {
            channel("Sends on", signalOut);
            switch (kind) {
            case Signals::Kind::PressureSensor:
                ui::SetNextItemWidth(160.0f);
                if (ui::InputInt("Pressure (%)", &signalB)) signalB = std::clamp(signalB, 0, 100);
                break;
            case Signals::Kind::MachineSensor: {
                const char* watches[] = { "a machine working", "a machine stopped", "a machine waiting for air", "a chest with anything in it", "an empty chest",
                                          "a tank or air network at least this full" };
                static_assert(UI_ARRAYSIZE(watches) == static_cast<int>(Signals::Watch::Count), "a watch has no name");
                signalModeIndex = std::clamp(signalModeIndex, 0, static_cast<int>(Signals::Watch::Count) - 1);
                ui::SetNextItemWidth(320.0f);
                ui::Combo("Watches for", &signalModeIndex, watches, UI_ARRAYSIZE(watches));
                if (signalModeIndex == static_cast<int>(Signals::Watch::Level)) {
                    ui::SetNextItemWidth(160.0f);
                    if (ui::InputInt("Full (%)", &signalB)) signalB = std::clamp(signalB, 0, 100);
                }
                break;
            }
            case Signals::Kind::HeatSensor:
                ui::SetNextItemWidth(160.0f);
                if (ui::InputInt("Range (blocks)", &signalB)) signalB = std::clamp(signalB, 0, Signals::kMaxHeatRange);
                break;
            case Signals::Kind::PlayerSensor:
                ui::SetNextItemWidth(160.0f);
                if (ui::InputInt("Range (blocks)", &signalB)) signalB = std::clamp(signalB, 0, Signals::kMaxPlayerRange);
                {
                    bool creaturesToo = signalModeIndex == static_cast<int>(Signals::Mode::Or);
                    if (ui::Checkbox("Hostile creatures too", &creaturesToo)) signalModeIndex = creaturesToo ? static_cast<int>(Signals::Mode::Or) : 0;
                }
                break;
            default: break;
            }
        }
        else {
            channel("Listens to", signalA);
            if (kind == Signals::Kind::Sprinkler) ui::TextDisabled("No channel: sprays when it senses fire");
            if (kind == Signals::Kind::Launcher) {
                ui::SetNextItemWidth(160.0f);
                if (ui::InputInt("Power (blocks/s)", &signalB)) signalB = signalB <= 0 ? 0 : std::clamp(signalB, Power::kMinLaunchSpeed, Power::kMaxLaunchSpeed);
                ui::TextDisabled("0: %d; %d to %d", static_cast<int>(Power::kLaunchSpeed), Power::kMinLaunchSpeed, Power::kMaxLaunchSpeed);
            }
        }
        if (ui::Button("Done") || ui::IsKeyPressed(ui::Key::Enter)) {
            net::SignalInfo info;
            info.pos = signalAt;
            info.kind = static_cast<uint8_t>(signalKind);
            info.mode = static_cast<uint8_t>(signalModeIndex);
            info.a = static_cast<uint8_t>(signalA);
            info.b = static_cast<uint8_t>(signalB);
            info.out = static_cast<uint8_t>(signalOut);
            client->ConfigureSignal(info);
            open = false;
        }
    }
    ui::End();
    if (!open) SetInventoryOpen(false);
}

void Application::DrawSignEditor() {
    const ui::Viewport* viewport = ui::GetMainViewport();
    ui::SetNextWindowPos(ui::Vec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.4f),
                            ui::Cond_Always, ui::Vec2(0.5f, 0.5f));
    ui::SetNextWindowBgAlpha(0.9f);
    bool open = true;
    if (ui::Begin("Sign", &open, ui::WindowFlags_AlwaysAutoResize | ui::WindowFlags_NoMove | ui::WindowFlags_NoSavedSettings | ui::WindowFlags_NoCollapse)) {
        ui::TextUnformatted("What's written on it (Enter to finish, Esc to leave it):");
        ui::SetNextItemWidth(420.0f);
        if (ui::IsWindowAppearing()) ui::SetKeyboardFocusHere();
        if (ui::InputText("##sign", signText, net::kMaxSignLength + 1, ui::InputTextFlags_EnterReturnsTrue)) {
            client->WriteSign(signAt, signText);
            open = false;
        }
    }
    ui::End();
    if (!open) SetInventoryOpen(false);
}

void Application::DrawSigns(const glm::mat4& viewProjection) {
    // Each sign's words over it, within 12 blocks and in sight, like a little plaque.
    ui::DrawList* draw = ui::GetBackgroundDrawList();
    const ui::Viewport* viewport = ui::GetMainViewport();
    const glm::vec3 eye = camera.GetPosition();
    for (const auto& [pos, text] : client->GetSigns()) {
        const glm::vec3 at = glm::vec3(pos) + glm::vec3(0.5f, 0.95f, 0.5f);
        const glm::vec3 to = at - eye;
        const float distance = glm::length(to);
        if (distance > 12.0f || distance < 0.3f) continue;
        const glm::vec4 clip = viewProjection * glm::vec4(at, 1.0f);
        if (clip.w <= 0.1f) continue;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        if (std::abs(ndc.x) > 1.1f || std::abs(ndc.y) > 1.1f) continue;
        if (auto hit = Raycast(*world, eye, to / distance, distance - 0.6f); hit && hit->block != pos) continue;
        const ui::Vec2 c(viewport->WorkPos.x + (ndc.x * 0.5f + 0.5f) * viewport->WorkSize.x, viewport->WorkPos.y + (0.5f - ndc.y * 0.5f) * viewport->WorkSize.y);
        const float fade = std::clamp((12.0f - distance) / 3.0f, 0.0f, 1.0f);
        const ui::Vec2 size = ui::CalcTextSize(text.c_str());
        const ui::Vec2 lo(c.x - size.x * 0.5f - 7.0f, c.y - size.y - 6.0f), hi(c.x + size.x * 0.5f + 7.0f, c.y + 2.0f);
        draw->AddRectFilled(lo, hi, UI_COL32(196, 160, 104, static_cast<int>(225 * fade)), 2.0f);
        draw->AddRect(lo, hi, UI_COL32(110, 76, 38, static_cast<int>(255 * fade)), 2.0f, 0, 2.0f);
        draw->AddText(ui::Vec2(lo.x + 7.0f, lo.y + 4.0f), UI_COL32(40, 26, 12, static_cast<int>(255 * fade)), text.c_str());
    }
}

Application::FightTarget Application::TargetInView() const {
    const glm::vec3 origin = camera.GetPosition(), dir = camera.GetDirection();
    float blockDistance = Combat::kReach;
    if (auto hit = Raycast(*world, origin, dir, Combat::kReach)) {
        // Where the ray enters the block's cell.
        const glm::vec3 lo(hit->block), hi = lo + glm::vec3(1.0f);
        float tNear = 0.0f;
        for (int a = 0; a < 3; a++) {
            if (std::abs(dir[a]) < 1e-6f) continue;
            const float t0 = (lo[a] - origin[a]) / dir[a], t1 = (hi[a] - origin[a]) / dir[a];
            tNear = std::max(tNear, std::min(t0, t1));
        }
        blockDistance = tNear;
    }
    // Where the ray enters a box, or -1 if it misses.
    auto enters = [&](const glm::vec3& lo, const glm::vec3& hi) {
        float tNear = 0.0f, tFar = 1e9f;
        for (int a = 0; a < 3; a++) {
            if (std::abs(dir[a]) < 1e-6f) {
                if (origin[a] < lo[a] || origin[a] > hi[a]) return -1.0f;
                continue;
            }
            float t0 = (lo[a] - origin[a]) / dir[a], t1 = (hi[a] - origin[a]) / dir[a];
            if (t0 > t1) std::swap(t0, t1);
            tNear = std::max(tNear, t0);
            tFar = std::min(tFar, t1);
        }
        return tNear <= tFar ? tNear : -1.0f;
    };
    FightTarget best;
    float bestT = blockDistance;
    for (const auto& c : client->GetCreatures()) {
        const CreatureSpec& spec = SpecOf(c.type);
        const float t = enters(c.feet - glm::vec3(spec.halfWidth, 0.0f, spec.halfWidth), c.feet + glm::vec3(spec.halfWidth, spec.height, spec.halfWidth));
        if (t >= 0.0f && t < bestT) bestT = t, best = FightTarget{ c.id, -1 };
    }
    for (const auto& p : client->GetRemotePlayers()) {
        const Collision::AABB body = Bodies::PlayerBox(p.feet, (p.flags & net::kFlagCrouching) != 0);
        const float t = enters(body.min, body.max);
        if (t >= 0.0f && t < bestT) bestT = t, best = FightTarget{ 0, p.id };
    }
    return best;
}

void Application::UpdateMining() {
    mineProgress = 0.0f;
    const bool held = (captureCursor && !chatOpen && !inventoryOpen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) || holdMine ||
                      (padMining && !chatOpen && !inventoryOpen && !paused);
    // Not while aiming at a creature (that's a fight, not mining).
    auto hit = held && client->IsWelcomed() && !client->IsCreative() && !TargetInView().Any()
                   ? Raycast(*world, camera.GetPosition(), camera.GetDirection(), Reach())
                   : std::nullopt;
    Block block;
    if (!hit || !world->GetBlock(hit->block.x, hit->block.y, hit->block.z, block) || block.IsAir() || block.IsLiquid()) {
        mining = false;
        return;
    }
    const Tool tool = client->Usable(SelectedTool()); // a tool not made yet is a bare hand
    const double now = glfwGetTime();
    // A new target (or tool, or the block changed under us): start over on it.
    if (!mining || hit->block != miningPos || tool != miningTool || block != miningBlock) {
        if (now < nextMineAt) return;
        mining = true;
        miningPos = hit->block;
        miningBlock = block;
        miningTool = tool;
        miningStart = now;
        client->StartMining(miningPos, tool);
    }
    const double needed = Mining::Seconds(block, tool, std::max(0, client->ToolTier(tool))), elapsed = now - miningStart;
    mineProgress = needed > 0.0 ? static_cast<float>(std::min(1.0, elapsed / needed)) : 1.0f;
    // A tap on the block every so often while mining it.
    if (needed > 0.0 && now >= nextDig) {
        const std::string material = Audio::Material(block.type);
        PlayAt(material == "hard" || material == "glass" ? "dig_hard" : "dig_soft", miningPos, 0.6f);
        nextDig = now + 0.27;
    }
    if (needed > 0.0 && now >= nextChip) {
        SpawnParticles(miningPos, block.type, 2, 1.5f); // chips flying off while it's mined
        nextChip = now + 0.12;
    }
    if (elapsed >= needed) {
        PlayAt(BreakSound(block.type), miningPos);
        SpawnParticles(miningPos, block.type, 18);
        // With an axe, the tree comes down: the logs above it break too.
        if (Trees::IsLog(block) && client->Usable(tool) == Tool::Axe)
            for (const auto& p : Trees::LogsAbove(*world, miningPos)) SpawnParticles(p, block.type, 10);
        client->BreakBlock(miningPos, tool);
        mining = false;
        mineProgress = 0.0f;
        nextMineAt = now + 0.15; // a moment before starting on the next block
    }
}

void Application::SpawnParticles(const glm::ivec3& pos, BlockType type, int count, float speed) {
    if (type == BlockType::AIR) return;
    static std::mt19937 rng(1234);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const TextureAtlas& atlas = renderer->GetAtlas();
    glm::vec2 uv0, uv1;
    atlas.GetTileUV(IconTile(type), uv0, uv1);
    for (int i = 0; i < count && particles.size() < 600; i++) {
        Particle p;
        p.position = glm::vec3(pos) + glm::vec3(0.2f + 0.6f * unit(rng), 0.2f + 0.6f * unit(rng), 0.2f + 0.6f * unit(rng));
        const glm::vec3 out = p.position - (glm::vec3(pos) + glm::vec3(0.5f));
        p.velocity = out * speed * 2.0f + glm::vec3(0.0f, speed * (0.4f + 0.6f * unit(rng)), 0.0f);
        p.maxLife = p.life = 0.5f + 0.6f * unit(rng);
        p.size = 0.07f + 0.07f * unit(rng);
        // A random quarter-by-quarter piece of the texture.
        const glm::vec2 span = (uv1 - uv0) * 0.25f, at = uv0 + (uv1 - uv0 - span) * glm::vec2(unit(rng), unit(rng));
        p.uv = glm::vec4(at, at + span);
        particles.push_back(p);
    }
}

void Application::UpdateLightning(float dt) {
    static std::mt19937 rng(77);
    std::uniform_real_distribution<float> spread(-1.0f, 1.0f);
    const glm::vec3 ear = camera.GetPosition();
    for (const glm::ivec3& at : client->TakeLightning()) {
        // A jagged bolt from high in the sky down to where it struck, with a fork.
        Bolt bolt{ {}, 0.35f };
        glm::vec3 p = glm::vec3(at) + glm::vec3(0.5f, 70.0f, 0.5f);
        const glm::vec3 ground = glm::vec3(at) + glm::vec3(0.5f, 0.0f, 0.5f);
        bolt.points.push_back(p);
        for (int i = 1; i <= 14; i++) {
            const float t = i / 14.0f;
            p = glm::mix(glm::vec3(ground.x, ground.y + 70.0f, ground.z), ground, t) + glm::vec3(spread(rng), 0.0f, spread(rng)) * 2.5f * (1.0f - t);
            if (i == 14) p = ground;
            bolt.points.push_back(p);
        }
        bolts.push_back(bolt);
        const float distance = glm::distance(ear, ground);
        lightningFlash = std::max(lightningFlash, std::clamp(1.2f - distance / 120.0f, 0.3f, 1.0f));
        // Thunder: later and quieter the further off (sound is slowed down a bit, so
        // it rolls in after the flash without a long wait).
        thunder.emplace_back(Client::Now() + distance / 120.0, std::clamp(1.4f - distance / 80.0f, 0.25f, 1.0f));
    }
    // Explosions: a boom, smoke and debris, and the view shaking if it's close.
    for (const auto& blast : client->TakeExplosions()) {
        const glm::vec3 centre = glm::vec3(blast.pos) + glm::vec3(0.5f);
        PlayAt("explosion", blast.pos, 1.0f);
        SpawnParticles(blast.pos, BlockType::STONE, 30, 9.0f);
        SpawnParticles(blast.pos, BlockType::DIRT, 20, 7.0f);
        glm::vec2 uv0, uv1;
        renderer->GetAtlas().GetTileUV(renderer->GetAtlas().GetIndex("smoke"), uv0, uv1);
        for (int i = 0; i < 40 && particles.size() < 600; i++) {
            Particle p;
            p.position = centre + glm::vec3(spread(rng), spread(rng), spread(rng)) * static_cast<float>(blast.radius) * 0.8f;
            p.velocity = (p.position - centre) * 0.8f;
            p.maxLife = p.life = 1.5f + std::abs(spread(rng)) * 1.5f;
            p.size = 0.35f + 0.25f * std::abs(spread(rng));
            p.uv = glm::vec4(uv0, uv1);
            p.rise = true;
            particles.push_back(p);
        }
        shake = std::max(shake, std::clamp(1.2f - glm::distance(ear, centre) / 24.0f, 0.0f, 1.0f));
        lightningFlash = std::max(lightningFlash, std::clamp(0.8f - glm::distance(ear, centre) / 40.0f, 0.0f, 0.8f));
    }
    shake = std::max(0.0f, shake - dt * 1.6f);
    for (auto& b : bolts) b.life -= dt;
    bolts.erase(std::remove_if(bolts.begin(), bolts.end(), [](const Bolt& b) { return b.life <= 0.0f; }), bolts.end());
    lightningFlash = std::max(0.0f, lightningFlash - dt * 3.0f);
    const double now = Client::Now();
    for (auto it = thunder.begin(); it != thunder.end();) {
        if (now >= it->first) {
            audio->PlayHere("thunder", it->second, 0.1f);
            it = thunder.erase(it);
        }
        else ++it;
    }
}

void Application::UpdateFireflies(float dt) {
    // Out on clear nights: up to 40 at a time, each living a while over a grassy spot
    // within 20 blocks, drifting around it and blinking.
    static std::mt19937 rng(4242);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float night = std::clamp((0.25f - std::sin(client->GetTimeOfDay() * 6.2831853f)) * 4.0f, 0.0f, 1.0f) * (1.0f - client->GetRain());
    const int wanted = static_cast<int>(40.0f * night);
    const glm::vec3 me = player.GetFeetPosition();
    for (int tries = 0; tries < 3 && static_cast<int>(fireflies.size()) < wanted; tries++) {
        const glm::ivec3 cell(glm::floor(me + glm::vec3(unit(rng) * 40.0f - 20.0f, unit(rng) * 8.0f - 3.0f, unit(rng) * 40.0f - 20.0f)));
        Block here, below;
        if (!world->GetBlock(cell.x, cell.y, cell.z, here) || !world->GetBlock(cell.x, cell.y - 1, cell.z, below)) continue;
        // Air (or a plant) over grass.
        if (!(here.IsAir() || here.IsPlant()) || !IsGrassType(below.type)) continue;
        const glm::vec3 home = glm::vec3(cell) + glm::vec3(unit(rng), 0.3f + unit(rng) * 1.2f, unit(rng));
        fireflies.push_back({ home, home, unit(rng) * 100.0f, 8.0f + 12.0f * unit(rng) });
    }
    for (auto& f : fireflies) {
        f.life -= dt * (night > 0.05f ? 1.0f : 6.0f); // (gone soon after dawn)
        f.phase += dt;
        f.position = f.home + glm::vec3(std::sin(f.phase * 0.7f) * 0.8f, std::sin(f.phase * 1.3f) * 0.3f, std::cos(f.phase * 0.5f) * 0.8f);
    }
    fireflies.erase(std::remove_if(fireflies.begin(), fireflies.end(),
                                   [&](const Firefly& f) { return f.life <= 0.0f || glm::distance(f.home, me) > 28.0f; }),
                    fireflies.end());
}

void Application::UpdateParticles(float dt) {
    // Smoke from campfires near the player (found by a look round every second).
    static std::mt19937 smokeRng(99);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const double now = Client::Now();
    if (now >= nextFireScan) {
        nextFireScan = now + 1.0;
        campfires.clear();
        rapids.clear();
        constexpr int kFull = Block::kMaxWaterVolume;
        // The water surface that water at `q`'s row would run onto there, in units from
        // the world's base: its own top surface, or, where the cell is open and dry, the
        // water just below it (a stream stepping down). False for a solid block, dry
        // ground, or water with more water above it (not a surface).
        auto surfaceAt = [&](const glm::ivec3& q, int& height) {
            Block n, other;
            if (!world->GetBlock(q.x, q.y, q.z, n)) return false;
            if (n.HasWater()) {
                if (world->GetBlock(q.x, q.y + 1, q.z, other) && other.HasWater()) return false;
                height = q.y * kFull + n.WaterShownLevel();
                return true;
            }
            if (!n.CanHoldWater() || !world->GetBlock(q.x, q.y - 1, q.z, other) || !other.HasWater()) return false;
            height = (q.y - 1) * kFull + other.WaterShownLevel();
            return true;
        };
        const glm::ivec3 c(glm::floor(player.GetFeetPosition()));
        for (int x = -16; x <= 16; x++)
            for (int z = -16; z <= 16; z++)
                for (int y = -6; y <= 6; y++) {
                    const glm::ivec3 p = c + glm::ivec3(x, y, z);
                    Block b;
                    if (!world->GetBlock(p.x, p.y, p.z, b)) continue;
                    if (b.type == BlockType::CAMPFIRE) {
                        campfires.push_back(p);
                        continue;
                    }
                    // Rapids: shallow water (a top layer lying on its bed, at most three
                    // quarters deep) whose surface stands well above the water beside it
                    // (a quarter of a block or more), so it's tumbling that way. A river's
                    // surface slopes gently all along its length, a little lower each
                    // cell, and that's just flowing, not rapids; water at rest is level,
                    // and dry ground beside a puddle doesn't count.
                    if (!b.HasWater() || rapids.size() >= 400) continue;
                    const int level = b.WaterShownLevel();
                    if (level > kFull * 3 / 4) continue;
                    Block above, below;
                    if (world->GetBlock(p.x, p.y + 1, p.z, above) && above.HasWater()) continue;
                    if (world->GetBlock(p.x, p.y - 1, p.z, below) && below.HasWater()) continue;
                    const int mine = p.y * kFull + level;
                    int bestDrop = 0;
                    glm::ivec2 bestDir(0);
                    for (const glm::ivec2 d : { glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1) }) {
                        int theirs;
                        if (surfaceAt(p + glm::ivec3(d.x, 0, d.y), theirs) && mine - theirs > bestDrop) bestDrop = mine - theirs, bestDir = d;
                    }
                    constexpr int kMinDrop = kFull / 4;
                    if (bestDrop < kMinDrop) continue;
                    rapids.push_back({ p, static_cast<float>(p.y) + level / static_cast<float>(kFull), glm::vec2(bestDir),
                                       std::clamp(static_cast<float>(bestDrop - kMinDrop) / (kFull / 2), 0.15f, 1.0f) });
                }
    }
    // Spray off the rapids: big white clouds of water thrown up and carried on
    // downstream, billowing as they go, with a few drops flung out of them; more, bigger
    // and faster where the water runs more steeply.
    if (!rapids.empty()) {
        static std::mt19937 sprayRng(4321);
        const TextureAtlas& atlas = renderer->GetAtlas();
        glm::vec2 cloud0, cloud1, water0, water1;
        atlas.GetTileUV(atlas.GetIndex("spray"), cloud0, cloud1);
        atlas.GetTileUV(IconTile(BlockType::WATER), water0, water1);
        float budget = dt * 6.0f * static_cast<float>(std::min<size_t>(rapids.size(), 48));
        while (budget > 0.0f && particles.size() < 600) {
            if (budget < 1.0f && unit(sprayRng) >= budget) break;
            budget -= 1.0f;
            const Rapid& r = rapids[static_cast<size_t>(unit(sprayRng) * rapids.size()) % rapids.size()];
            if (unit(sprayRng) > 0.3f + 0.7f * r.strength) continue; // gentle runs spray less
            const glm::vec2 across(-r.direction.y, r.direction.x);
            Particle p;
            p.position = glm::vec3(r.cell.x + 0.1f + 0.8f * unit(sprayRng), r.surfaceY + 0.05f, r.cell.z + 0.1f + 0.8f * unit(sprayRng));
            if (unit(sprayRng) < 0.75f) {
                // A cloud: a third of a block or more to start, growing as it drifts.
                const float speed = (0.4f + 1.4f * r.strength) * (0.6f + 0.8f * unit(sprayRng));
                const glm::vec2 h = r.direction * speed + across * (unit(sprayRng) - 0.5f) * 0.5f;
                p.velocity = glm::vec3(h.x, 0.3f + 0.6f * unit(sprayRng), h.y);
                p.maxLife = p.life = 1.0f + 0.9f * unit(sprayRng);
                p.size = (0.28f + 0.22f * unit(sprayRng)) * (0.7f + 0.6f * r.strength);
                p.uv = glm::vec4(cloud0, cloud1);
                p.mist = true;
            }
            else {
                // A drop flung out: small, falling.
                const float speed = (0.5f + 2.5f * r.strength) * (0.6f + 0.8f * unit(sprayRng));
                const glm::vec2 h = r.direction * speed + across * (unit(sprayRng) - 0.5f) * 0.8f;
                p.velocity = glm::vec3(h.x, (1.0f + 1.6f * unit(sprayRng)) * (0.6f + 0.6f * r.strength), h.y);
                p.maxLife = p.life = 0.35f + 0.35f * unit(sprayRng);
                p.size = 0.05f + 0.05f * unit(sprayRng);
                const glm::vec2 span = (water1 - water0) * 0.25f, at = water0 + (water1 - water0 - span) * glm::vec2(unit(sprayRng), unit(sprayRng));
                p.uv = glm::vec4(at, at + span);
            }
            particles.push_back(p);
        }
    }
    // Fires: licks of flame shooting up from each near us, smoke now and then, and a
    // crackle from one of them every so often.
    {
        static std::mt19937 fireRng(2024);
        glm::vec2 f0, f1, s0, s1;
        renderer->GetAtlas().GetTileUV(renderer->GetAtlas().GetIndex("flame"), f0, f1);
        renderer->GetAtlas().GetTileUV(renderer->GetAtlas().GetIndex("smoke"), s0, s1);
        const glm::vec3 eye = camera.GetPosition();
        std::vector<glm::ivec3> fires;
        for (const auto& pos : world->GetModelBlocks()) {
            Block b;
            if (world->GetBlock(pos.x, pos.y, pos.z, b) && b.type == BlockType::FIRE && glm::distance(glm::vec3(pos), eye) < 48.0f) fires.push_back(pos);
        }
        if (fires.size() > 400) fires.resize(400);
        // Other players on fire: flames up their bodies.
        for (const auto& other : client->GetRemotePlayers()) {
            if (!(other.flags & net::kFlagBurning) || glm::distance(other.feet, eye) > 48.0f) continue;
            for (int i = 0; i < 2 && particles.size() < 3000; i++) {
                if (unit(fireRng) >= dt * 30.0f) continue;
                Particle p;
                p.position = other.feet + glm::vec3(0.6f * (unit(fireRng) - 0.5f), 1.7f * unit(fireRng), 0.6f * (unit(fireRng) - 0.5f));
                p.velocity = glm::vec3(0.0f, 1.2f + unit(fireRng), 0.0f);
                p.maxLife = p.life = 0.3f + 0.3f * unit(fireRng);
                p.size = 0.3f + 0.2f * unit(fireRng);
                p.uv = glm::vec4(f0, f1);
                p.flame = true;
                particles.push_back(p);
            }
        }
        for (const auto& pos : fires) {
            float rate = dt * 14.0f;
            while (rate > 0.0f && particles.size() < 3000) {
                if (rate < 1.0f && unit(fireRng) >= rate) break;
                rate -= 1.0f;
                Particle p;
                p.position = glm::vec3(pos) + glm::vec3(0.15f + 0.7f * unit(fireRng), 0.1f + 0.2f * unit(fireRng), 0.15f + 0.7f * unit(fireRng));
                p.velocity = glm::vec3(0.3f * (unit(fireRng) - 0.5f), 1.6f + 1.4f * unit(fireRng), 0.3f * (unit(fireRng) - 0.5f));
                p.maxLife = p.life = 0.35f + 0.35f * unit(fireRng);
                p.size = 0.35f + 0.3f * unit(fireRng);
                p.uv = glm::vec4(f0, f1);
                p.flame = true;
                particles.push_back(p);
            }
            if (unit(fireRng) < dt * 1.5f && particles.size() < 3000) {
                Particle s;
                s.position = glm::vec3(pos) + glm::vec3(0.5f, 0.9f, 0.5f);
                s.velocity = glm::vec3(0.2f * (unit(fireRng) - 0.5f), 0.0f, 0.2f * (unit(fireRng) - 0.5f));
                s.maxLife = s.life = 2.0f + unit(fireRng);
                s.size = 0.25f + 0.2f * unit(fireRng);
                s.uv = glm::vec4(s0, s1);
                s.rise = true;
                particles.push_back(s);
            }
        }
        if (!fires.empty() && now >= nextCrackle) {
            const glm::ivec3 at = fires[static_cast<size_t>(unit(fireRng) * fires.size()) % fires.size()];
            PlayAt("fire_crackle", at, std::min(1.0f, 0.4f + 0.05f * static_cast<float>(fires.size())));
            nextCrackle = now + std::max(0.15, 0.9 - 0.02 * static_cast<double>(fires.size()));
        }
    }
    if (!campfires.empty() && unit(smokeRng) < dt * 5.0f * std::min<size_t>(campfires.size(), 4)) {
        const glm::ivec3 fire = campfires[static_cast<size_t>(unit(smokeRng) * campfires.size()) % campfires.size()];
        glm::vec2 uv0, uv1;
        renderer->GetAtlas().GetTileUV(renderer->GetAtlas().GetIndex("smoke"), uv0, uv1);
        Particle p;
        p.position = glm::vec3(fire) + glm::vec3(0.35f + 0.3f * unit(smokeRng), 0.6f, 0.35f + 0.3f * unit(smokeRng));
        p.velocity = glm::vec3(0.2f * (unit(smokeRng) - 0.5f), 0.0f, 0.2f * (unit(smokeRng) - 0.5f));
        p.maxLife = p.life = 2.5f + unit(smokeRng);
        p.size = 0.08f + 0.07f * unit(smokeRng);
        p.uv = glm::vec4(uv0, uv1);
        p.rise = true;
        if (particles.size() < 600) particles.push_back(p);
    }
    // Gravity, a little air drag, and they come to rest on whatever's under them
    // (smoke rises instead, slowly).
    for (auto& p : particles) {
        p.life -= dt;
        if (p.flame) {
            // Shooting up and slowing (shrinking away as it's drawn).
            p.velocity *= std::exp(-1.5f * dt);
            p.position += p.velocity * dt;
            continue;
        }
        if (p.rise) {
            p.velocity.y += (1.1f - p.velocity.y) * std::min(1.0f, dt * 2.0f);
            p.position += p.velocity * dt;
            continue;
        }
        if (p.mist) {
            // Slowing as it spreads, and lifting gently: no falling, no bouncing.
            p.velocity *= std::exp(-1.4f * dt);
            p.velocity.y += (0.25f - p.velocity.y) * std::min(1.0f, dt * 1.5f);
            p.position += p.velocity * dt;
            continue;
        }
        p.velocity.y -= 18.0f * dt;
        p.velocity *= std::exp(-1.5f * dt);
        glm::vec3 next = p.position + p.velocity * dt;
        Block below;
        const glm::ivec3 cell(glm::floor(next - glm::vec3(0.0f, p.size * 0.5f, 0.0f)));
        if (world->GetBlock(cell.x, cell.y, cell.z, below) && !below.IsPassable() && below.IsFullBlock()) {
            next.y = std::max(next.y, static_cast<float>(cell.y + 1) + p.size * 0.5f);
            p.velocity.y = -p.velocity.y * 0.25f;
            p.velocity.x *= 0.6f;
            p.velocity.z *= 0.6f;
        }
        p.position = next;
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.life <= 0.0f; }), particles.end());
}

const char* Application::BreakSound(BlockType type) {
    if (type == BlockType::PEBBLES) return "pickup";
    const std::string material = Audio::Material(type);
    if (material == "hard") return "break_hard";
    if (material == "wood") return "break_wood";
    if (material == "glass") return "break_glass";
    return "break_soft";
}

void Application::PlayAt(const char* name, const glm::ivec3& block, float volume) {
    audio->Play(name, glm::vec3(block) + glm::vec3(0.5f), volume);
}

void Application::UpdateSounds() {
    audio->SetVolume(mute ? 0.0f : settings.volume);
    audio->SetListener(camera.GetPosition(), camera.GetDirection(), glm::vec3(0.0f, 1.0f, 0.0f));
    if (!joined) return;
    const double now = glfwGetTime();
    // What's underfoot decides how a step sounds.
    auto stepAt = [&](const glm::vec3& feet, float volume) {
        Block below;
        const glm::ivec3 cell(glm::floor(feet - glm::vec3(0.0f, 0.1f, 0.0f)));
        if (!world->GetBlock(cell.x, cell.y, cell.z, below) || below.IsAir() || below.IsLiquid()) return;
        audio->Play(std::string("step_") + (std::string(Audio::Material(below.type)) == "glass" ? "hard" : Audio::Material(below.type)), feet, volume);
    };

    // Our own steps: one every 1.7 blocks walked on the ground (a jump far away, a
    // teleport, doesn't count).
    const glm::vec3 feet = player.GetFeetPosition();
    const float moved = glm::length(glm::vec2(feet.x - lastFeet.x, feet.z - lastFeet.z));
    if (player.IsOnGround() && !player.IsFlying() && !player.IsSwimming() && moved < 2.0f) {
        stepDistance += moved;
        if (stepDistance >= 1.7f) {
            stepDistance = 0.0f;
            stepAt(feet, 0.35f);
        }
    }
    lastFeet = feet;
    // Into the water.
    if (player.IsSwimming() && !wasSwimming) {
        audio->Play("splash", feet, 0.6f);
        // Spray, more the harder you hit the water.
        const float speed = std::abs(player.GetVelocity().y);
        SpawnParticles(glm::ivec3(glm::floor(feet)), BlockType::WATER, std::clamp(static_cast<int>(speed * 2.5f), 4, 24), 2.0f + speed * 0.3f);
    }
    wasSwimming = player.IsSwimming();
    // Hurt.
    if (client->GetLastHurt() > lastHurtHeard + 0.05) {
        lastHurtHeard = client->GetLastHurt();
        audio->PlayHere("hurt", 0.8f);
    }

    // Ambience outdoors (open sky, not raining hard): birds by day, crickets at night,
    // now and then, from somewhere around.
    if (now >= nextAmbience) {
        static std::mt19937 ambienceRng(808);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        nextAmbience = now + 5.0 + 9.0 * unit(ambienceRng);
        const float sun = std::sin(client->GetTimeOfDay() * 6.2831853f);
        if (!sheltered && client->GetRain() < 0.3f && std::abs(sun) > 0.15f) {
            const float a = unit(ambienceRng) * 6.2831853f;
            const glm::vec3 at = camera.GetPosition() + glm::vec3(std::cos(a) * 9.0f, 2.0f + 3.0f * unit(ambienceRng), std::sin(a) * 9.0f);
            audio->Play(sun > 0.0f ? "birds" : "crickets", at, 0.35f, 0.08f);
        }
    }

    // What others broke and built (a few at a time: a collapse or the laser makes many).
    int played = 0;
    for (const auto& event : client->TakeBlockEvents()) {
        // A piston's head shooting out or pulling back: a pneumatic thunk, not a block placed or broken.
        if (event.type == BlockType::PISTON_HEAD) {
            if (played++ < 4) PlayAt("piston", event.pos, 0.7f);
            continue;
        }
        if (played < 4) PlayAt(event.placed ? "place" : BreakSound(event.type), event.pos, 0.8f);
        if (!event.placed && played < 24 && event.type != BlockType::WATER) SpawnParticles(event.pos, event.type, played < 4 ? 14 : 4);
        played++;
    }

    // Other players' steps.
    for (auto& [id, w] : walkers) w.seen = false;
    for (const auto& other : client->GetRemotePlayers()) {
        Walker& w = walkers[other.id];
        // A thump when they're hit (by us, or anything else).
        const bool hurt = (other.flags & net::kFlagHurt) != 0;
        if (hurt && !w.hurt) audio->Play("hit", other.feet + glm::vec3(0.0f, 1.0f, 0.0f), 0.9f);
        w.hurt = hurt;
        const float step = glm::length(glm::vec2(other.feet.x - w.feet.x, other.feet.z - w.feet.z));
        if (w.seen || w.distance > 0.0f || step < 2.0f) {
            if ((other.flags & net::kFlagOnGround) && step < 2.0f) w.distance += step;
            if (w.distance >= 1.7f) {
                w.distance = 0.0f;
                stepAt(other.feet, 0.4f);
            }
        }
        w.feet = other.feet;
        w.seen = true;
    }
    for (auto it = walkers.begin(); it != walkers.end();) it = it->second.seen ? std::next(it) : walkers.erase(it);

    // Creatures: a thump when hit, and now and then a hostile dwarf's grunt.
    std::uniform_real_distribution<double> wait(4.0, 11.0);
    static std::mt19937 rng(99);
    for (auto& [id, c] : creatureSounds) c.seen = false;
    for (const auto& c : client->GetCreatures()) {
        CreatureSound& s = creatureSounds[c.id];
        if (c.hurt && !s.hurt) audio->Play("hit", c.feet + glm::vec3(0.0f, 0.8f, 0.0f), 0.9f);
        s.hurt = c.hurt;
        if (SpecOf(c.type).hostile) {
            if (s.nextGrunt == 0.0) s.nextGrunt = now + wait(rng) * 0.5;
            else if (now >= s.nextGrunt) {
                audio->Play("grunt", c.feet + glm::vec3(0.0f, 1.2f, 0.0f), c.type == CreatureType::DwarfChief ? 1.0f : 0.7f, 0.15f);
                s.nextGrunt = now + wait(rng);
            }
        }
        s.seen = true;
    }
    for (auto it = creatureSounds.begin(); it != creatureSounds.end();) it = it->second.seen ? std::next(it) : creatureSounds.erase(it);

    // Rain, as heard here: muffled under cover (a quarter as loud with no open sky near).
    audio->SetLoop("rain_loop", rainHere * 0.45f * (0.25f + 0.75f * rainOpen));
}

void Application::OnCursorMove(double x, double y) {
    if (captureCursor && !ignoreMouse) camera.UpdateLook(x, y);
}

void Application::OnScroll(double yOffset) {
    // With the cursor released the wheel belongs to the UI.
    if (!captureCursor) return;
    // Trackpads send many small steps; change slot once per whole notch.
    scrollAccumulator += yOffset;
    while (scrollAccumulator >= 1.0) { SelectSlot(selectedSlot - 1); scrollAccumulator -= 1.0; }
    while (scrollAccumulator <= -1.0) { SelectSlot(selectedSlot + 1); scrollAccumulator += 1.0; }
}

void Application::ScrollCallback(GLFWwindow* window, double, double yOffset) {
    ui::AddMouseWheel(static_cast<float>(yOffset));
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnScroll(yOffset);
}

void Application::KeyCallback(GLFWwindow* window, int key, int, int action, int mods) {
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        const bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
        if (ctrl && key == GLFW_KEY_V) ui::AddChar(22); // (paste)
        switch (key) {
        case GLFW_KEY_ENTER:
        case GLFW_KEY_KP_ENTER: ui::AddKey(ui::Key::Enter); break;
        case GLFW_KEY_ESCAPE: ui::AddKey(ui::Key::Escape); break;
        case GLFW_KEY_BACKSPACE: ui::AddKey(ui::Key::Backspace); break;
        case GLFW_KEY_DELETE: ui::AddKey(ui::Key::Delete); break;
        case GLFW_KEY_LEFT: ui::AddKey(ui::Key::Left); break;
        case GLFW_KEY_RIGHT: ui::AddKey(ui::Key::Right); break;
        case GLFW_KEY_UP: ui::AddKey(ui::Key::Up); break;
        case GLFW_KEY_DOWN: ui::AddKey(ui::Key::Down); break;
        case GLFW_KEY_HOME: ui::AddKey(ui::Key::Home); break;
        case GLFW_KEY_END: ui::AddKey(ui::Key::End); break;
        case GLFW_KEY_TAB: ui::AddKey(ui::Key::Tab); break;
        default: break;
        }
    }
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnKey(key, action);
}

void Application::CharCallback(GLFWwindow*, unsigned int codepoint) { ui::AddChar(codepoint); }

void Application::MouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    ui::AddMouseButton(button, action == GLFW_PRESS);
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnMouseButton(button, action);
}

void Application::CursorPosCallback(GLFWwindow* window, double x, double y) {
    ui::SetMousePos({ static_cast<float>(x), static_cast<float>(y) });
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnCursorMove(x, y);
}

void Application::BeginUiFrame() {
    int w = 0, h = 0;
    glfwGetWindowSize(window, &w, &h);
    const double now = glfwGetTime();
    const float dt = uiLastFrame > 0.0 ? static_cast<float>(std::min(0.25, now - uiLastFrame)) : 1.0f / 60.0f;
    uiLastFrame = now;
    // A gamepad moves between the menus' buttons (D-pad), presses (A).
    GLFWgamepadstate pad;
    if (glfwJoystickIsGamepad(GLFW_JOYSTICK_1) && glfwGetGamepadState(GLFW_JOYSTICK_1, &pad))
        ui::SetGamepad(pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_UP] == GLFW_PRESS, pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_DOWN] == GLFW_PRESS,
                       pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_LEFT] == GLFW_PRESS, pad.buttons[GLFW_GAMEPAD_BUTTON_DPAD_RIGHT] == GLFW_PRESS,
                       pad.buttons[GLFW_GAMEPAD_BUTTON_A] == GLFW_PRESS, pad.buttons[GLFW_GAMEPAD_BUTTON_B] == GLFW_PRESS);
    else ui::SetGamepad(false, false, false, false, false, false);
    ui::NewFrame({ static_cast<float>(std::max(1, w)), static_cast<float>(std::max(1, h)) }, dt);
}

void Application::EndUiFrame() {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ui::Render({ static_cast<float>(w), static_cast<float>(h) });
}

float Application::Reach() const {
    return client && client->IsCreative() ? kCreativeReach : kSurvivalReach;
}
