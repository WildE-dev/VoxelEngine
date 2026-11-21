#include "app/Application.h"

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
#include <random>
#include <string>
#include <vector>

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "audio/Audio.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "net/Protocol.h"
#include "render/Renderer.h"
#include "server/Server.h"
#include "world/Achievements.h"
#include "world/Breathing.h"
#include "world/Hunger.h"
#include "world/Power.h"
#include "world/Signals.h"
#include "world/Chunk.h"
#include "world/Crafting.h"
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
// Everything the inventory (I) offers, in the order it shows them.
constexpr BlockType kInventoryBlocks[] = {
    BlockType::DIRT, BlockType::GRASS, BlockType::DRY_GRASS, BlockType::LUSH_GRASS, BlockType::SNOWY_GRASS, BlockType::STONE, BlockType::SAND,
    BlockType::SNOW, BlockType::ICE, BlockType::LOG, BlockType::LEAVES, BlockType::PINE_LEAVES, BlockType::CACTUS, BlockType::LAMP,
    BlockType::STONE_BRICK, BlockType::MOSSY_BRICK, BlockType::CRUMBLING_BRICK, BlockType::WATER, BlockType::PUMP, BlockType::PIPE,
    BlockType::WATER_WHEEL, BlockType::SHAFT, BlockType::COMPRESSOR, BlockType::AIR_PIPE, BlockType::AIR_TANK, BlockType::VALVE, BlockType::DRILL, BlockType::LEVER, BlockType::LOGIC_GATE, BlockType::SIGNAL_LAMP, BlockType::SLUICE, BlockType::PRESSURE_PLATE, BlockType::SPIKES, BlockType::TALL_GRASS, BlockType::FLOWER_RED, BlockType::FLOWER_YELLOW,
    BlockType::DEAD_BUSH, BlockType::PEBBLES, BlockType::PLANKS, BlockType::GLASS, BlockType::CHEST, BlockType::BED, BlockType::LADDER, BlockType::DOOR, BlockType::CAMPFIRE, BlockType::SIGN, BlockType::TNT, BlockType::SAPLING, BlockType::PINE_SAPLING, BlockType::COAL_ORE, BlockType::IRON_ORE, BlockType::RAW_VENISON, BlockType::COOKED_VENISON, BlockType::HIDE, BlockType::COAL, BlockType::IRON_INGOT, BlockType::SEEDS, BlockType::WHEAT, BlockType::BREAD, BlockType::ARROW, BlockType::RAW_FISH, BlockType::COOKED_FISH, BlockType::BOAT,
};
// Where the chosen hotbar is kept between games (working directory).
constexpr const char* kHotbarFile = "hotbar.txt";

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

// The tools the inventory offers, and the icon (atlas tile) for one.
constexpr Tool kInventoryTools[] = { Tool::Pickaxe, Tool::Axe, Tool::Shovel, Tool::Sword, Tool::Bow, Tool::Rod };
int ToolTile(const TextureAtlas& atlas, Tool tool) {
    const char* name = Mining::ToolTexture(tool);
    return name ? atlas.GetIndex(name) : 0;
}
const char* ToolHint(Tool tool) {
    switch (tool) {
    case Tool::Pickaxe: return "fast on stone, bricks, ice and machines";
    case Tool::Axe: return "fast on wood";
    case Tool::Shovel: return "fast on dirt, grass, sand and snow";
    case Tool::Sword: return "for fighting: 3.5 hearts a hit (and quick on leaves)";
    case Tool::Bow: return "right-click to shoot an arrow (2.5 hearts); needs arrows";
    case Tool::Rod: return "right-click water to cast; wait for a bite (stay close)";
    default: return "";
    }
}

int IconTile(BlockType type) {
    const TextureData t = GetTextureData(type);
    return IsPlantType(type) ? t.top : t.front;
}

const char* BlockName(BlockType type) {
    switch (type) {
    case BlockType::GRASS: return "Grass";
    case BlockType::DIRT:  return "Dirt";
    case BlockType::STONE: return "Stone";
    case BlockType::LAMP:  return "Lamp";
    case BlockType::LOG:   return "Log";
    case BlockType::LEAVES: return "Leaves";
    case BlockType::TALL_GRASS: return "Tall grass";
    case BlockType::FLOWER_RED: return "Red flower";
    case BlockType::FLOWER_YELLOW: return "Yellow flower";
    case BlockType::SAND:  return "Sand";
    case BlockType::WATER: return "Water";
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
    case BlockType::VALVE: return "Valve (closed)";
    case BlockType::VALVE_OPEN: return "Valve (open)";
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
    case BlockType::RAW_VENISON: return "Raw venison";
    case BlockType::COOKED_VENISON: return "Cooked venison";
    case BlockType::HIDE: return "Hide";
    case BlockType::CHEST: return "Chest";
    case BlockType::BED: return "Bed";
    case BlockType::COAL_ORE: return "Coal ore";
    case BlockType::IRON_ORE: return "Iron ore";
    case BlockType::COAL: return "Coal";
    case BlockType::IRON_INGOT: return "Iron ingot";
    case BlockType::SEEDS: return "Seeds";
    case BlockType::CROP: return "Young crop";
    case BlockType::RIPE_WHEAT: return "Ripe wheat";
    case BlockType::WHEAT: return "Wheat";
    case BlockType::BREAD: return "Bread";
    case BlockType::LADDER: return "Ladder";
    case BlockType::DOOR: return "Door";
    case BlockType::DOOR_OPEN: return "Open door";
    case BlockType::ARROW: return "Arrow";
    case BlockType::CAMPFIRE: return "Campfire";
    case BlockType::SIGN: return "Sign";
    case BlockType::TNT: return "TNT";
    case BlockType::TNT_LIT: return "Lit TNT";
    case BlockType::BOAT: return "Boat";
    case BlockType::SAPLING: return "Sapling";
    case BlockType::PINE_SAPLING: return "Pine sapling";
    case BlockType::RAW_FISH: return "Raw fish";
    case BlockType::COOKED_FISH: return "Cooked fish";
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

    // Installed before ImGui so its backend chains to ours.
    glfwSetWindowUserPointer(window, this);
    glfwSetKeyCallback(window, KeyCallback);
    glfwSetMouseButtonCallback(window, MouseButtonCallback);
    glfwSetCursorPosCallback(window, CursorPosCallback);
    glfwSetScrollCallback(window, ScrollCallback);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(kGlslVersion);
    imguiInitialized = true;

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
    LoadBlockTextures();
    LoadHotbar();
    // VOXEL_OPEN_INVENTORY=1 starts with the inventory open (for screenshots).
    if (std::getenv("VOXEL_OPEN_INVENTORY")) SetInventoryOpen(true);
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
                 : name == "settings" ? Page::Settings : Page::Title;
        }
        return true;
    }
    ClientOptions start = options;
    if (!start.nameGiven) start.name = settings.name;
    return StartGame(start);
}

bool Application::StartGame(const ClientOptions& options) {
    menuMessage.clear();
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
    }
    else {
        serverLabel = host + ":" + std::to_string(port);
    }

    std::string error;
    if (!client->Connect(host, port, options.name, error, settings.renderDistance)) {
        std::cerr << "ERROR: Could not connect to " << host << ":" << port << ": " << error << std::endl;
        menuMessage = "Couldn't connect to " + host + ":" + std::to_string(port) + ": " + error;
        StopGame();
        return false;
    }
    paused = false;
    SetCursorCaptured(true);
    // VOXEL_PAUSE=1 starts with the pause menu open (for screenshots).
    if (std::getenv("VOXEL_PAUSE")) {
        paused = true;
        SetCursorCaptured(false);
    }
    return true;
}

void Application::StopGame() {
    if (client) client->Disconnect();
    client.reset();
    world.reset();
    if (localServer) localServer->Stop(); // (saves the world)
    localServer.reset();
    joined = false;
    mining = false;
    walkers.clear();
    creatureSounds.clear();
    if (audio) audio->SetLoop("rain_loop", 0.0f);
    paused = pauseSettings = false;
    chatOpen = inventoryOpen = false;
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
    ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard); // the menus type and click
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    if (!client && !autoScreenshotPath.empty() && now >= autoScreenshotAt) {
        SaveScreenshot(autoScreenshotPath, width, height);
        autoScreenshotPath.clear();
    }
    glfwSwapInterval(vsync ? 1 : 0);
    glfwSwapBuffers(window);
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
    // Order matters: everything holding GL objects goes before the context.
    if (mapTexture) {
        glDeleteTextures(1, &mapTexture);
        mapTexture = 0;
    }
    audio.reset();
    if (client) client->Disconnect();
    client.reset();
    world.reset();
    renderer.reset();
    if (localServer) localServer->Stop();
    localServer.reset();

    if (imguiInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
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
        auto lap = [&](int section) {
            if (!bench) return;
            const double t = glfwGetTime();
            benchSections[section] += t - mark;
            mark = t;
        };
        UpdateNetwork();
        lap(0);
        if (rightClickAt > 0.0 && glfwGetTime() >= rightClickAt) {
            rightClickAt = 0.0;
            OnMouseButton(GLFW_MOUSE_BUTTON_RIGHT, GLFW_PRESS);
        }
        HandleContinuousInput(deltaTime);
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
        world->Update(camera.GetPosition());
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
            if (world->GetBlock(hit->block.x, hit->block.y, hit->block.z, target)) params.highlightOutline = target.OutlineSegments();
        }
        if (mining && mineProgress > 0.0f) {
            // Cracks over the box the block's shape fills.
            glm::vec3 lo(1.0f), hi(0.0f);
            for (const glm::vec3& p : miningBlock.OutlineSegments()) lo = glm::min(lo, p), hi = glm::max(hi, p);
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
                if (GetLightEmission(other.heldBlock) > 0)
                    params.handLights.push_back(glm::vec4(other.feet + glm::vec3(0.0f, 1.2f, 0.0f), static_cast<float>(GetLightEmission(other.heldBlock))));
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
            params.creatures.push_back({ static_cast<uint8_t>(c.type), c.feet, c.yaw, c.hurt, c.speed, lightAt(c.feet), c.tame });
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
            params.particles.push_back({ at, 0.18f, glm::vec4(uv0, uv1), lightAt(at) });
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
        UpdateMachines(params, lightAt);
        for (const auto& p : particles)
            params.particles.push_back({ p.position, p.size * std::min(1.0f, p.life / (0.4f * p.maxLife)), p.uv, lightAt(p.position - glm::vec3(0.0f, 0.1f, 0.0f)) });
        const TextureAtlas& atlas = renderer->GetAtlas();
        for (const auto& other : client->GetRemotePlayers()) {
            PlayerModel model{ other.feet, other.yaw, other.pitch, PlayerModels::ColorFor(other.id), (other.flags & net::kFlagHurt) != 0, lightAt(other.feet) };
            model.crouching = (other.flags & net::kFlagCrouching) != 0;
            model.boat = (other.flags & net::kFlagBoat) != 0;
            // What's in their hand: a tool's or an item's icon as a card, a block as a
            // little cube.
            int tile = -1;
            if (other.heldTool != Tool::None) tile = ToolTile(atlas, other.heldTool), model.held = PlayerModel::Held::Icon;
            else if (other.heldBlock != BlockType::AIR) {
                tile = IconTile(other.heldBlock);
                const bool flat = IsItemType(other.heldBlock) || IsPlantType(other.heldBlock) || Block(other.heldBlock).IsLiquid();
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
        ImGuiIO& io = ImGui::GetIO();
        if (captureCursor) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        // The keyboard is the UI's only while something takes typing (chat, the pause
        // menu's settings, the inventory); otherwise it's the game's.
        if (chatOpen || paused || inventoryOpen) io.ConfigFlags &= ~ImGuiConfigFlags_NoKeyboard;
        else io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        DrawNameTags(params.projection * params.view);
        DrawSigns(params.projection * params.view);
        if (showCompass) DrawCompass();
        if (showMap) DrawMinimap();
        DrawToast();
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
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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
            if (now - benchLast >= 1.0) {
                const double n = benchFrames;
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
    const float target = (sheltered || params.underwater) ? 0.0f : rain;
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
    client->Poll();

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
        // Corrected into terrain (e.g. back to the spawn point): climb out onto it.
        if (!player.IsFlying() && Collision::BoxBlocked(*world, player.GetBounds())) player.ResetSpawn();
    }
}

void Application::DrawCrosshair() {
    // Only while aiming: not with the cursor free, the inventory open or typing.
    if (!captureCursor || inventoryOpen || chatOpen || !client->IsWelcomed()) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 c(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    constexpr float kArm = 9.0f, kGap = 0.0f;
    // A dark outline under a white cross, so it shows on sky and snow alike.
    for (const auto& [color, width] : { std::pair<ImU32, float>{ IM_COL32(0, 0, 0, 170), 4.0f }, { IM_COL32(255, 255, 255, 230), 2.0f } }) {
        draw->AddLine(ImVec2(c.x - kArm, c.y), ImVec2(c.x - kGap, c.y), color, width);
        draw->AddLine(ImVec2(c.x + kGap, c.y), ImVec2(c.x + kArm, c.y), color, width);
        draw->AddLine(ImVec2(c.x, c.y - kArm), ImVec2(c.x, c.y - kGap), color, width);
        draw->AddLine(ImVec2(c.x, c.y + kGap), ImVec2(c.x, c.y + kArm), color, width);
    }
    // Looking at a signal block: its channels and whether it's on.
    Block target;
    auto label = [&](const char* text) {
        const ImVec2 size = ImGui::CalcTextSize(text);
        const ImVec2 p(c.x - size.x * 0.5f, c.y + 22.0f);
        draw->AddRectFilled(ImVec2(p.x - 6, p.y - 3), ImVec2(p.x + size.x + 6, p.y + size.y + 3), IM_COL32(0, 0, 0, 140), 4.0f);
        draw->AddText(p, IM_COL32(255, 255, 255, 235), text);
    };
    if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
        at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && Signals::KindOf(target.type) != Signals::Kind::None) {
        const auto& infos = client->GetSignals();
        auto it = infos.find(at->block);
        char text[160];
        const net::SignalInfo info = it != infos.end() ? it->second : net::SignalInfo{};
        auto ch = [](int c) { return c ? std::to_string(c) : std::string("none"); };
        switch (Signals::KindOf(target.type)) {
        case Signals::Kind::Lever: std::snprintf(text, sizeof text, "Lever: %s - sends on %s", target.type == BlockType::LEVER_ON ? "on" : "off", ch(info.out).c_str()); break;
        case Signals::Kind::Plate: std::snprintf(text, sizeof text, "Pressure plate - sends on %s", ch(info.out).c_str()); break;
        case Signals::Kind::Gate: {
            const auto mode = static_cast<Signals::Mode>(info.mode);
            const std::string inputs = Signals::Inputs(mode) == 2 ? ch(info.a) + ", " + ch(info.b) : ch(info.a);
            std::snprintf(text, sizeof text, "%s gate (%s) -> %s: %s", Signals::ModeName(mode), inputs.c_str(), ch(info.out).c_str(), info.on ? "on" : "off");
            break;
        }
        case Signals::Kind::Lamp: std::snprintf(text, sizeof text, "Signal lamp - listens to %s", ch(info.a).c_str()); break;
        case Signals::Kind::Valve:
            std::snprintf(text, sizeof text, "Valve: %s - %s", target.type == BlockType::VALVE_OPEN ? "open" : "shut",
                          info.a ? ("listens to " + ch(info.a)).c_str() : "by hand");
            break;
        default: std::snprintf(text, sizeof text, "Sluice gate - %s", info.a ? ("listens to " + ch(info.a)).c_str() : "no channel (stays shut)"); break;
        }
        label(text);
        return;
    }
    // Looking at a water wheel or a machine: how it's doing, under the crosshair.
    if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
        at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && Power::IsMachine(target.type)) {
        for (const auto& m : client->GetMachines()) {
            if (m.pos != at->block) continue;
            char text[128];
            const int pressure = static_cast<int>(std::lround(m.pressure / 2.55f));
            const auto state = static_cast<Power::DrillState>(m.state);
            if (target.type == BlockType::WATER_WHEEL)
                std::snprintf(text, sizeof text, state == Power::DrillState::Blocked ? "Water wheel: jammed" : m.speed ? "Water wheel: turning (%d%%)" : "Water wheel: still",
                              int(std::lround(m.speed / 2.55f)));
            else if (target.type == BlockType::COMPRESSOR)
                std::snprintf(text, sizeof text, "Compressor: +%.1f air/s from %d wheel%s - air %d%%", m.speed / 2.0f, m.depth, m.depth == 1 ? "" : "s", pressure);
            else if (target.type == BlockType::AIR_TANK) std::snprintf(text, sizeof text, "Air tank - air %d%%", pressure);
            else if (target.type == BlockType::PUMP)
                std::snprintf(text, sizeof text, "Pump: %s - air %d%%", state == Power::DrillState::Working ? "pumping" : state == Power::DrillState::NoPressure ? "needs air pressure" : "nothing to pump", pressure);
            else std::snprintf(text, sizeof text, "Drill: %s - air %d%%", Power::Describe(state), pressure);
            const ImVec2 size = ImGui::CalcTextSize(text);
            const ImVec2 p(c.x - size.x * 0.5f, c.y + 22.0f);
            draw->AddRectFilled(ImVec2(p.x - 6, p.y - 3), ImVec2(p.x + size.x + 6, p.y + size.y + 3), IM_COL32(0, 0, 0, 140), 4.0f);
            draw->AddText(p, IM_COL32(255, 255, 255, 235), text);
            break;
        }
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

    // Wheels and shafts in lines along their axles turn together, as fast as the
    // fastest wheel on the line (still if the server said nothing). Each line keeps its
    // angle under the position of its first block.
    const std::vector<glm::ivec3> models = world->GetModelBlocks();
    std::unordered_map<glm::ivec3, Block, Coords::IVec3Hash> blocks;
    for (const auto& pos : models) {
        Block b;
        if (world->GetBlock(pos.x, pos.y, pos.z, b) && b.IsModel()) blocks[pos] = b;
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
}

namespace {
// The colour a block shows from above on the minimap (0xAABBGGRR).
uint32_t MapColour(BlockType type) {
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
    case BlockType::CROP:
    case BlockType::RIPE_WHEAT: return 0xFF46B4C8u;
    default: return 0xFF707070u;
    }
}
} // namespace

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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kMapSize, kMapSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, mapPixels.data());
        mapCentre = here;
    }
    // Moved far from the picture's centre: start again around here.
    if (std::abs(here.x - mapCentre.x) > kMapSize / 6 || std::abs(here.y - mapCentre.y) > kMapSize / 6) {
        mapCentre = here;
        mapRow = 0;
    }
    // A few rows a frame: the topmost block of each column, from a little above the
    // player down (so caves and overhangs below show the ground over them), shaded by
    // whether the ground rises or falls toward the north.
    const int top = static_cast<int>(feet.y) + 24, bottom = static_cast<int>(feet.y) - 40;
    for (int rows = 0; rows < 6; rows++, mapRow = (mapRow + 1) % kMapSize) {
        const int z = mapCentre.y - kMapSize / 2 + mapRow;
        int previous = -100000;
        for (int col = 0; col < kMapSize; col++) {
            const int x = mapCentre.x - kMapSize / 2 + col;
            uint32_t colour = 0xFF202020u;
            int height = bottom;
            for (int y = top; y >= bottom; y--) {
                Block b;
                if (!world->GetBlock(x, y, z, b)) break;
                if (b.IsAir() || (b.IsPlant() && b.type != BlockType::CROP && b.type != BlockType::RIPE_WHEAT)) continue;
                colour = MapColour(b.type);
                height = y;
                break;
            }
            // Relief: a column higher than the one north of it is lit, lower is shaded.
            Block north;
            int northHeight = height;
            for (int y = top; y >= bottom; y--) {
                if (!world->GetBlock(x, y, z - 1, north)) break;
                if (!north.IsAir() && !north.IsPlant()) { northHeight = y; break; }
            }
            const float shade = std::clamp(1.0f + 0.12f * (height - northHeight), 0.65f, 1.3f);
            auto channel = [&](int shift) { return static_cast<uint32_t>(std::min(255.0f, ((colour >> shift) & 0xFF) * shade)) << shift; };
            mapPixels[mapRow * kMapSize + col] = 0xFF000000u | channel(0) | channel(8) | channel(16);
            (void)previous;
        }
        glBindTexture(GL_TEXTURE_2D, mapTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, mapRow, kMapSize, 1, GL_RGBA, GL_UNSIGNED_BYTE, &mapPixels[mapRow * kMapSize]);
    }
    // Shown in the top-right corner, north up, with an arrow for the player.
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float kShown = 170.0f;
    const ImVec2 lo(viewport->WorkPos.x + viewport->WorkSize.x - kShown - 12.0f, viewport->WorkPos.y + 12.0f), hi(lo.x + kShown, lo.y + kShown);
    draw->AddRectFilled(ImVec2(lo.x - 3.0f, lo.y - 3.0f), ImVec2(hi.x + 3.0f, hi.y + 3.0f), IM_COL32(0, 0, 0, 150), 4.0f);
    draw->AddImage(static_cast<ImTextureID>(static_cast<uintptr_t>(mapTexture)), lo, hi);
    const float scale = kShown / kMapSize;
    const ImVec2 me(lo.x + (feet.x - (mapCentre.x - kMapSize / 2)) * scale, lo.y + (feet.z - (mapCentre.y - kMapSize / 2)) * scale);
    // Other players: dots.
    for (const auto& other : client->GetRemotePlayers()) {
        const ImVec2 at(lo.x + (other.feet.x - (mapCentre.x - kMapSize / 2)) * scale, lo.y + (other.feet.z - (mapCentre.y - kMapSize / 2)) * scale);
        if (at.x > lo.x && at.x < hi.x && at.y > lo.y && at.y < hi.y) draw->AddCircleFilled(at, 3.5f, IM_COL32(255, 220, 80, 255));
    }
    const float yaw = glm::radians(camera.GetYaw());
    const ImVec2 f(std::cos(yaw), std::sin(yaw)), r(-f.y, f.x);
    draw->AddTriangleFilled(ImVec2(me.x + f.x * 7.0f, me.y + f.y * 7.0f), ImVec2(me.x - f.x * 4.0f + r.x * 4.5f, me.y - f.y * 4.0f + r.y * 4.5f),
                            ImVec2(me.x - f.x * 4.0f - r.x * 4.5f, me.y - f.y * 4.0f - r.y * 4.5f), IM_COL32(255, 255, 255, 255));
    draw->AddText(ImVec2(lo.x + kShown * 0.5f - 4.0f, lo.y + 2.0f), IM_COL32(255, 110, 100, 255), "N");
}

void Application::DrawPlayerList() {
    // Everyone here: you first, then the others with how far off they are and which way.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.6f);
    if (ImGui::Begin("Players", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoFocusOnAppearing)) {
        const auto others = client->GetRemotePlayers();
        ImGui::Text("Players online: %d", static_cast<int>(others.size()) + 1);
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "%s (you)", client->GetMyName().c_str());
        const glm::vec3 me = player.GetFeetPosition();
        for (const auto& o : others) {
            const glm::vec3 d = o.feet - me;
            const float bearing = std::fmod(glm::degrees(std::atan2(d.z, d.x)) + 90.0f + 720.0f, 360.0f);
            static const char* kWays[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
            ImGui::Text("%s   %d blocks %s", o.name.empty() ? "?" : o.name.c_str(), static_cast<int>(glm::length(d)),
                        kWays[static_cast<int>((bearing + 22.5f) / 45.0f) % 8]);
        }
    }
    ImGui::End();
}

void Application::DrawDeath() {
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
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 lo = viewport->WorkPos, hi(lo.x + viewport->WorkSize.x, lo.y + viewport->WorkSize.y);
    draw->AddRectFilledMultiColor(lo, hi, IM_COL32(90, 0, 0, static_cast<int>(150 * fade)), IM_COL32(90, 0, 0, static_cast<int>(150 * fade)),
                                  IM_COL32(20, 0, 0, static_cast<int>(200 * fade)), IM_COL32(20, 0, 0, static_cast<int>(200 * fade)));
    const char* title = "You died!";
    const float cx = lo.x + viewport->WorkSize.x * 0.5f, cy = lo.y + viewport->WorkSize.y * 0.38f;
    ImFont* font = ImGui::GetFont();
    const float big = ImGui::GetFontSize() * 2.6f;
    const ImVec2 titleSize = font->CalcTextSizeA(big, FLT_MAX, 0.0f, title);
    draw->AddText(font, big, ImVec2(cx - titleSize.x * 0.5f + 3.0f, cy + 3.0f), IM_COL32(0, 0, 0, static_cast<int>(200 * fade)), title);
    draw->AddText(font, big, ImVec2(cx - titleSize.x * 0.5f, cy), IM_COL32(255, 90, 80, static_cast<int>(255 * fade)), title);
    if (!how.empty()) {
        const ImVec2 size = ImGui::CalcTextSize(how.c_str());
        draw->AddText(ImVec2(cx - size.x * 0.5f, cy + titleSize.y + 14.0f), IM_COL32(255, 230, 220, static_cast<int>(240 * fade)), how.c_str());
    }
}

void Application::DrawToast() {
    // Achievements arrive as chat lines starting with the toast mark: the newest shows
    // as a banner under the compass for four seconds, sliding in and fading out.
    const auto& chat = client->GetChat();
    const std::string mark = Achievements::kToastMark;
    if (chat.size() < chatSeen) chatSeen = 0;
    for (; chatSeen < chat.size(); chatSeen++)
        if (chat[chatSeen].text.rfind(mark, 0) == 0) {
            toastText = chat[chatSeen].text.substr(mark.size());
            toastUntil = Client::Now() + 4.0;
            audio->PlayHere("craft", 0.7f);
        }
    const double left = toastUntil - Client::Now();
    if (left <= 0.0 || toastText.empty()) return;
    const float fade = static_cast<float>(std::min(1.0, std::min(left, 4.0 - left) * 3.0));
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const std::string title = "Achievement unlocked";
    const ImVec2 textSize = ImGui::CalcTextSize(toastText.c_str()), titleSize = ImGui::CalcTextSize(title.c_str());
    const float width = std::max(textSize.x, titleSize.x) + 40.0f, cx = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f;
    const float y = viewport->WorkPos.y + 62.0f - (1.0f - fade) * 20.0f;
    const ImVec2 lo(cx - width * 0.5f, y), hi(cx + width * 0.5f, y + titleSize.y + textSize.y + 18.0f);
    draw->AddRectFilled(lo, hi, IM_COL32(30, 24, 8, static_cast<int>(210 * fade)), 6.0f);
    draw->AddRect(lo, hi, IM_COL32(240, 200, 80, static_cast<int>(255 * fade)), 6.0f, 0, 2.0f);
    draw->AddText(ImVec2(cx - titleSize.x * 0.5f, y + 6.0f), IM_COL32(255, 214, 90, static_cast<int>(255 * fade)), title.c_str());
    draw->AddText(ImVec2(cx - textSize.x * 0.5f, y + 10.0f + titleSize.y), IM_COL32(255, 255, 255, static_cast<int>(240 * fade)), toastText.c_str());
}

void Application::DrawCompass() {
    // A strip across the top of the screen showing the way you face: N, E, S and W
    // (north is -z) with ticks between, sliding as you turn; under it, where you are
    // and the time of day.
    if (!client->IsWelcomed()) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = 360.0f, cx = viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, top = viewport->WorkPos.y + 10.0f;
    const float bearing = std::fmod(camera.GetYaw() + 90.0f + 720.0f, 360.0f); // degrees clockwise from north
    draw->AddRectFilled(ImVec2(cx - width * 0.5f, top), ImVec2(cx + width * 0.5f, top + 26.0f), IM_COL32(0, 0, 0, 90), 4.0f);
    draw->PushClipRect(ImVec2(cx - width * 0.5f, top), ImVec2(cx + width * 0.5f, top + 26.0f), true);
    constexpr float kPixelsPerDegree = 2.0f; // 180 degrees across
    for (int deg = 0; deg < 360; deg += 15) {
        float offset = std::fmod(deg - bearing + 540.0f, 360.0f) - 180.0f;
        const float x = cx + offset * kPixelsPerDegree;
        if (std::abs(offset) > width * 0.5f / kPixelsPerDegree + 10.0f) continue;
        const char* label = deg == 0 ? "N" : deg == 90 ? "E" : deg == 180 ? "S" : deg == 270 ? "W" : nullptr;
        if (label) {
            const ImVec2 size = ImGui::CalcTextSize(label);
            draw->AddText(ImVec2(x - size.x * 0.5f, top + 5.0f), deg == 0 ? IM_COL32(255, 110, 100, 255) : IM_COL32(255, 255, 255, 235), label);
        }
        else draw->AddLine(ImVec2(x, top + (deg % 45 == 0 ? 7.0f : 11.0f)), ImVec2(x, top + 19.0f), IM_COL32(255, 255, 255, deg % 45 == 0 ? 200 : 120), 1.5f);
    }
    draw->PopClipRect();
    draw->AddTriangleFilled(ImVec2(cx - 5.0f, top + 26.0f), ImVec2(cx + 5.0f, top + 26.0f), ImVec2(cx, top + 20.0f), IM_COL32(255, 230, 120, 255));
    // Where, and when (the day runs from sunrise at 06:00).
    const glm::vec3 feet = player.GetFeetPosition();
    const float hours = std::fmod(client->GetTimeOfDay() * 24.0f + 6.0f, 24.0f);
    const bool day = client->GetTimeOfDay() < 0.5f;
    char line[96];
    std::snprintf(line, sizeof line, "%d, %d, %d    %s %02d:%02d", static_cast<int>(std::floor(feet.x)), static_cast<int>(std::floor(feet.y)),
                  static_cast<int>(std::floor(feet.z)), day ? "sun" : "moon", static_cast<int>(hours), static_cast<int>(std::fmod(hours, 1.0f) * 60.0f));
    const ImVec2 size = ImGui::CalcTextSize(line);
    draw->AddText(ImVec2(cx - size.x * 0.5f + 1.0f, top + 31.0f), IM_COL32(0, 0, 0, 200), line);
    draw->AddText(ImVec2(cx - size.x * 0.5f, top + 30.0f), day ? IM_COL32(255, 244, 200, 235) : IM_COL32(190, 210, 255, 235), line);
}

void Application::DrawNameTags(const glm::mat4& viewProjection) {
    // Each other player's name over their head: within 40 blocks, in view, and not
    // behind a wall (the line from the eye to the tag is clear), fading with distance.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
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
        const ImVec2 at(viewport->WorkPos.x + (ndc.x * 0.5f + 0.5f) * viewport->WorkSize.x,
                        viewport->WorkPos.y + (0.5f - ndc.y * 0.5f) * viewport->WorkSize.y);
        const float fade = std::clamp((kRange - distance) / 10.0f, 0.0f, 1.0f);
        const ImVec2 size = ImGui::CalcTextSize(other.name.c_str());
        const ImVec2 lo(at.x - size.x * 0.5f - 5.0f, at.y - size.y - 4.0f), hi(at.x + size.x * 0.5f + 5.0f, at.y + 2.0f);
        draw->AddRectFilled(lo, hi, IM_COL32(0, 0, 0, static_cast<int>(120 * fade)), 3.0f);
        draw->AddText(ImVec2(lo.x + 5.0f, lo.y + 3.0f), IM_COL32(255, 255, 255, static_cast<int>(235 * fade)), other.name.c_str());
    }
}

void Application::DrawHealth() {
    if (!client->IsWelcomed()) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

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
        const ImVec2 lo = viewport->WorkPos, hi(lo.x + viewport->WorkSize.x, lo.y + viewport->WorkSize.y);
        const bool water = cause == Breathing::Cause::Water;
        const int r = water ? 0 : 10, g = water ? 20 : 8, b = water ? 60 : 12;
        const ImU32 edge = IM_COL32(r, g, b, static_cast<int>(230 * strength)), clear = IM_COL32(r, g, b, 0);
        const float band = std::min(viewport->WorkSize.x, viewport->WorkSize.y) * (0.15f + 0.3f * strength);
        draw->AddRectFilledMultiColor(lo, ImVec2(hi.x, lo.y + band), edge, edge, clear, clear);
        draw->AddRectFilledMultiColor(ImVec2(lo.x, hi.y - band), hi, clear, clear, edge, edge);
        draw->AddRectFilledMultiColor(lo, ImVec2(lo.x + band, hi.y), edge, clear, clear, edge);
        draw->AddRectFilledMultiColor(ImVec2(hi.x - band, lo.y), hi, clear, edge, edge, clear);
    }

    const float sinceHurt = static_cast<float>(Client::Now() - client->GetLastHurt());
    if (sinceHurt < 0.5f) {
        const int alpha = static_cast<int>(110.0f * (1.0f - sinceHurt / 0.5f));
        draw->AddRectFilled(viewport->WorkPos, ImVec2(viewport->WorkPos.x + viewport->WorkSize.x, viewport->WorkPos.y + viewport->WorkSize.y),
                            IM_COL32(200, 20, 20, alpha));
    }
    if (player.IsFlying()) return; // no-clip: nothing can hurt you

    // Hearts above the hotbar's left end: full, half or empty.
    constexpr float kSize = 9.0f, kGap = 4.0f;
    const int hearts = (client->GetMaxHealth() + 1) / 2, health = client->GetHealth();
    const float hotbarWidth = kHotbarSlots * 52.0f + (kHotbarSlots - 1) * 4.0f + 16.0f;
    const float x0 = viewport->WorkPos.x + (viewport->WorkSize.x - hotbarWidth) * 0.5f + 6.0f;
    const float y = viewport->WorkPos.y + viewport->WorkSize.y - 12.0f - 98.0f;
    auto heart = [&](float cx, float cy, ImU32 color) {
        // Two lobes and a point.
        draw->AddCircleFilled(ImVec2(cx - kSize * 0.45f, cy), kSize * 0.55f, color);
        draw->AddCircleFilled(ImVec2(cx + kSize * 0.45f, cy), kSize * 0.55f, color);
        draw->AddTriangleFilled(ImVec2(cx - kSize, cy + kSize * 0.15f), ImVec2(cx + kSize, cy + kSize * 0.15f), ImVec2(cx, cy + kSize * 1.1f), color);
    };
    const bool blink = sinceHurt < 0.6f && static_cast<int>(sinceHurt * 10.0f) % 2 == 0;
    for (int i = 0; i < hearts; i++) {
        const float cx = x0 + kSize + i * (2.0f * kSize + kGap);
        heart(cx + 1.5f, y + 1.5f, IM_COL32(0, 0, 0, 160));       // shadow
        heart(cx, y, IM_COL32(60, 50, 50, 220));                    // empty
        const int left = health - 2 * i;
        const ImU32 red = blink ? IM_COL32(255, 200, 200, 255) : IM_COL32(220, 30, 40, 255);
        if (left >= 2) heart(cx, y, red);
        else if (left == 1) {
            draw->PushClipRect(ImVec2(cx - kSize * 1.2f, y - kSize), ImVec2(cx, y + kSize * 1.2f), true); // left half
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
        auto drumstick = [&](float cx, float cy, ImU32 meat, ImU32 bone) {
            // The meat, upper left; the bone sticking out to the lower right with a knob.
            draw->AddLine(ImVec2(cx, cy), ImVec2(cx + kSize * 0.8f, cy + kSize * 0.8f), bone, kSize * 0.35f);
            draw->AddCircleFilled(ImVec2(cx + kSize * 0.85f, cy + kSize * 0.6f), kSize * 0.22f, bone);
            draw->AddCircleFilled(ImVec2(cx + kSize * 0.6f, cy + kSize * 0.85f), kSize * 0.22f, bone);
            draw->AddCircleFilled(ImVec2(cx - kSize * 0.2f, cy - kSize * 0.2f), kSize * 0.7f, meat);
        };
        for (int i = 0; i < sticks; i++) {
            const float cx = right - kSize * 1.4f - i * (2.0f * kSize + kGap), cy = y + shiver * ((i % 2) ? 1.0f : -1.0f);
            drumstick(cx + 1.5f, cy + 1.5f, IM_COL32(0, 0, 0, 160), IM_COL32(0, 0, 0, 160)); // shadow
            drumstick(cx, cy, IM_COL32(60, 45, 35, 220), IM_COL32(70, 65, 60, 220));          // empty
            const int left = food - 2 * i;
            const ImU32 meat = IM_COL32(176, 96, 44, 255), bone = IM_COL32(235, 225, 205, 255);
            if (left >= 2) drumstick(cx, cy, meat, bone);
            else if (left == 1) {
                draw->PushClipRect(ImVec2(cx, cy - kSize * 1.2f), ImVec2(cx + kSize * 1.4f, cy + kSize * 1.4f), true); // the bone half
                drumstick(cx, cy, meat, bone);
                draw->PopClipRect();
            }
        }
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
                draw->AddCircleFilled(ImVec2(cx, y + 2.0f), kSize * 0.75f, IM_COL32(20, 30, 45, 150)); // gone
                draw->AddCircle(ImVec2(cx, y + 2.0f), kSize * 0.75f, IM_COL32(140, 180, 220, 170), 0, 1.5f);
                continue;
            }
            const float radius = kSize * (left >= 2 ? 0.8f : 0.5f); // half a bubble is a small one
            draw->AddCircleFilled(ImVec2(cx + 1.5f, y + 3.5f), radius, IM_COL32(0, 0, 0, 140));
            draw->AddCircleFilled(ImVec2(cx, y + 2.0f), radius, IM_COL32(90, 170, 255, 235));
            draw->AddCircle(ImVec2(cx, y + 2.0f), radius, IM_COL32(220, 240, 255, 255), 0, 1.5f);
            draw->AddCircleFilled(ImVec2(cx - radius * 0.35f, y + 2.0f - radius * 0.35f), radius * 0.25f, IM_COL32(255, 255, 255, 230)); // shine
        }
        if (cause != Breathing::Cause::None) {
            const char* label = CauseName(cause);
            const ImVec2 size = ImGui::CalcTextSize(label);
            const ImVec2 at(right - size.x, y - kSize - size.y - 4.0f);
            draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 220), label);
            draw->AddText(at, air == 0 ? IM_COL32(255, 120, 110, 255) : IM_COL32(200, 230, 255, 255), label);
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

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float width = std::min(560.0f, viewport->WorkSize.x - 24.0f);
    // Above the hotbar and the hearts and air bubbles over it.
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + viewport->WorkSize.y - 160.0f), ImGuiCond_Always,
                            ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::SetNextWindowBgAlpha(chatOpen ? 0.7f : 0.55f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f)); // black, so text reads over bright terrain
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    const bool visible = ImGui::Begin("Chat", nullptr, flags);
    ImGui::PopStyleColor(2);
    if (!visible) {
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(width - 16.0f);
    for (size_t i = first; i < lines.size(); i++) {
        const auto& line = lines[i];
        ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
        switch (line.kind) {
        case net::ChatMessage::Kind::Info: color = ImVec4(1.0f, 0.88f, 0.45f, 1.0f); break;
        case net::ChatMessage::Kind::Error: color = ImVec4(1.0f, 0.5f, 0.45f, 1.0f); break;
        case net::ChatMessage::Kind::Private: color = ImVec4(0.82f, 0.65f, 1.0f, 1.0f); break;
        default: break;
        }
        if (!chatOpen) color.w = static_cast<float>(std::clamp((kShowSeconds - (now - line.time)) / kFadeSeconds, 0.0, 1.0));
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(line.text.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();

    if (chatOpen) {
        if (chatOpenFrames < 2) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0f);
        // Just opened: the key that opened it may also arrive as a typed character (drop
        // the 't', keep a single '/'), and the box would select what's in it (the '/'),
        // so the cursor goes to the end with nothing selected. Done inside the box's
        // own edit (a callback): it keeps its own copy of the text while typing.
        struct Opening {
            char with;
            int frames;
        } opening{ chatOpenedWith, chatOpenFrames };
        auto onEdit = [](ImGuiInputTextCallbackData* data) {
            const auto* o = static_cast<const Opening*>(data->UserData);
            if (o->frames >= 3) return 0;
            const std::string text(data->Buf, data->BufTextLen);
            if (o->with == 't' && (text == "t" || text == "T")) data->DeleteChars(0, data->BufTextLen);
            if (o->with == '/' && text == "//") data->DeleteChars(1, 1);
            data->ClearSelection();
            data->CursorPos = data->BufTextLen;
            return 0;
        };
        const bool send = ImGui::InputText("##chat", chatInput, sizeof chatInput, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways,
                                           onEdit, &opening);
        chatOpenFrames++;
        if (send) {
            client->SendChat(chatInput);
            CloseChat();
        }
    }
    ImGui::End();
}

void Application::DrawStatus() {
    std::string text;
    if (!client->IsConnected() && !client->GetDisconnectReason().empty()) text = "Disconnected: " + client->GetDisconnectReason();
    else if (!client->IsWelcomed()) text = "Connecting to " + serverLabel + "...";
    else if (!player.IsSpawned() && !player.IsFlying()) text = "Loading terrain...";
    if (text.empty()) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + 40.0f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("Status", nullptr, flags)) ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void Application::HandleContinuousInput(float deltaTime) {
    auto down = [&](int key) { return glfwGetKey(window, key) == GLFW_PRESS; };
    if (!joined) return;

    // The player drops onto the terrain once the chunks under it have loaded.
    if (!player.IsSpawned() && !player.IsFlying()) player.Spawn(*world);

    // No-clip is the server's to switch (/noclip): follow it.
    const bool noclip = client->GetAbilities() & net::kAbilityNoclip;
    if (noclip != player.IsFlying()) player.SetFlying(*world, noclip);

    PlayerInput input;
    if (!chatOpen && !inventoryOpen && !paused) {
    input.move = glm::vec2((down(GLFW_KEY_D) ? 1.0f : 0.0f) - (down(GLFW_KEY_A) ? 1.0f : 0.0f),
                           (down(GLFW_KEY_W) ? 1.0f : 0.0f) - (down(GLFW_KEY_S) ? 1.0f : 0.0f));
    input.jump = down(GLFW_KEY_SPACE) || (player.IsFlying() && down(GLFW_KEY_E));
    input.descend = down(GLFW_KEY_Q) || down(GLFW_KEY_LEFT_CONTROL);
    input.crouch = down(GLFW_KEY_LEFT_CONTROL) && !player.IsFlying(); // (flying, Ctrl goes down)
    // In a boat, Ctrl gets out (and doesn't crouch).
    if (player.IsBoating() && input.crouch) {
        player.SetBoating(false);
        input.crouch = false;
    }
    // Short of breath (thin or stale air, half the air gone): too winded to sprint.
    const Breathing::Cause cause = client->GetAirCause();
    const bool winded = (cause == Breathing::Cause::ThinAir || cause == Breathing::Cause::StaleAir) && client->GetAir() < Breathing::kShortOfBreath;
    input.sprint = down(GLFW_KEY_LEFT_SHIFT) && !winded;
    }
    input.yaw = camera.GetYaw();
    input.lookDirection = camera.GetDirection();
    player.Update(*world, input, deltaTime);

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
        const Tool heldTool = client->HasTool(SelectedTool()) ? SelectedTool() : Tool::None;
        BlockType heldBlock = SelectedTool() == Tool::None ? SelectedBlock() : BlockType::AIR;
        if (!client->IsCreative() && heldBlock != BlockType::AIR && client->GetResources(heldBlock) == 0) heldBlock = BlockType::AIR;
        client->SendPlayerState(player.GetFeetPosition(), player.GetVelocity(), camera.GetYaw(), camera.GetDirectionAngles().x, flags, heldTool,
                                heldBlock);
    }

    UpdateMining();

    // Laser: carve a tunnel along the view ray while L is held.
    if (captureCursor && !chatOpen && (client->GetAbilities() & net::kAbilityOperator) && glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
        client->Carve(TraceBlocks(*world, camera.GetPosition(), camera.GetDirection(), kLaserRange), kLaserRadius);
    }
}

void Application::DrawHotbar() {
    constexpr int kSlots = kHotbarSlots;
    constexpr float kIcon = 40.0f, kPad = 6.0f, kGap = 4.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float barWidth = kSlots * (kIcon + 2 * kPad) + (kSlots - 1) * kGap;

    // A borderless strip along the bottom centre of the screen.
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y - 12.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(kGap, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("Hotbar", nullptr, flags)) {
        // The selected item's name, centred above the slots, with the height building
        // blocks are placed at (V); reddish if it can't be used (a tool not made yet,
        // or a block we haven't enough of).
        const BlockType selected = SelectedBlock();
        const Tool selectedTool = SelectedTool();
        const bool survival = !client->IsCreative();
        std::string label = selectedTool != Tool::None ? Mining::ToolName(selectedTool) : BlockName(selected);
        if (selectedTool == Tool::None && Block::CanShape(selected))
            label += std::string(" - ") + HeightName(placeHeight) + (placeHeight < 8 ? " height" : " block") + " (V)";
        const bool affordable = selectedTool != Tool::None ? client->HasTool(selectedTool)
                                                           : client->CanAfford(selected, Block::CanShape(selected) ? placeHeight : 8);
        const bool affordableShown = affordable;
        const float nameWidth = ImGui::CalcTextSize(label.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (barWidth - nameWidth) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, affordableShown ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f) : ImVec4(1.0f, 0.55f, 0.5f, 1.0f));
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();

        const TextureAtlas& atlas = renderer->GetAtlas();
        const ImTextureID atlasTexture = static_cast<ImTextureID>(static_cast<uintptr_t>(atlas.GetTexture()));
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int i = 0; i < kSlots; i++) {
            if (i > 0) ImGui::SameLine();
            const bool selected = i == selectedSlot;
            const ImVec2 slotMin = ImGui::GetCursorScreenPos();
            const ImVec2 slotMax(slotMin.x + kIcon + 2 * kPad, slotMin.y + kIcon + 2 * kPad);

            // Clicking a slot selects it (when the cursor is released with Tab).
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("slot", ImVec2(slotMax.x - slotMin.x, slotMax.y - slotMin.y))) SelectSlot(i);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            draw->AddRectFilled(slotMin, slotMax, selected ? IM_COL32(255, 255, 255, 60) : hovered ? IM_COL32(255, 255, 255, 30) : IM_COL32(0, 0, 0, 70), 4.0f);
            // The block's side texture, straight from the world's atlas.
            glm::vec2 uv0, uv1;
            const bool isTool = hotbarTools[i] != Tool::None;
            atlas.GetTileUV(isTool ? ToolTile(atlas, hotbarTools[i]) : IconTile(hotbar[i]), uv0, uv1);
            const uint32_t held = isTool ? 0 : client->GetResources(hotbar[i]);
            const bool empty = survival && (isTool ? !client->HasTool(hotbarTools[i]) : held == 0);
            draw->AddImage(atlasTexture, ImVec2(slotMin.x + kPad, slotMin.y + kPad), ImVec2(slotMax.x - kPad, slotMax.y - kPad),
                           ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y), empty ? IM_COL32(255, 255, 255, 70) : IM_COL32_WHITE);
            // Survival: how many we have, in the bottom right.
            if (survival && held > 0) {
                const std::string count = FormatBlocks(held);
                const ImVec2 size = ImGui::CalcTextSize(count.c_str());
                const ImVec2 at(slotMax.x - size.x - 4.0f, slotMax.y - size.y - 2.0f);
                draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 230), count.c_str());
                draw->AddText(at, IM_COL32(255, 255, 255, 255), count.c_str());
            }
            draw->AddRect(slotMin, slotMax, selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 60), 4.0f, 0, selected ? 3.0f : 1.0f);
            // Number key in the corner (1-9, then 0; later slots are picked with the wheel).
            char key[2] = { static_cast<char>(i < 9 ? '1' + i : i == 9 ? '0' : 0), 0 };
            draw->AddText(ImVec2(slotMin.x + 5.0f, slotMin.y + 3.0f), IM_COL32(0, 0, 0, 200), key); // shadow, readable on light textures
            draw->AddText(ImVec2(slotMin.x + 4.0f, slotMin.y + 2.0f), IM_COL32(255, 255, 255, selected ? 255 : 190), key);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
}

void Application::DrawDebugWindow(float deltaTime) {
    frameSampleTimer += deltaTime;
    if (frameSampleTimer > 0.01f) {
        frameTimes[frameTimeIndex] = deltaTime * 1000.0f;
        frameTimeIndex = (frameTimeIndex + 1) % kFrameHistory;
        frameSampleTimer = 0.0f;
    }

    if (ImGui::Begin("Debug", &showDebug)) {
        const glm::vec3 pos = camera.GetPosition();
        const glm::vec2 angles = camera.GetDirectionAngles();

        ImGui::Checkbox("VSync", &vsync);
        ImGui::Checkbox("Wireframe", &wireframe);
        ImGui::Checkbox("Ambient occlusion", &ambientOcclusion);
        {
            const float t = client->GetTimeOfDay();
            const int minutes = static_cast<int>((t * 24.0f + 6.0f) * 60.0f) % (24 * 60); // sunrise at 06:00
            ImGui::Text("Time %02d:%02d (N skips to dawn / dusk), rain %.0f%%%s", minutes / 60, minutes % 60, client->GetRain() * 100.0f,
                        sheltered ? ", sheltered" : "");
        }
        ImGui::Text("Placing: %s", BlockName(SelectedBlock()));
        ImGui::Text("Position %.3f, %.3f, %.3f", pos.x, pos.y, pos.z);
        if (!(client->GetSpawnFlags() & net::kWelcomeEmptyWorld)) {
            // The world generator is deterministic, so the biome can be worked out here.
            static const TerrainGenerator climate;
            const int bx = static_cast<int>(std::floor(pos.x)), bz = static_cast<int>(std::floor(pos.z));
            const int row = climate.ColumnAt(bx, bz).surfaceRow;
            const auto c = climate.ClimateAt(bx, bz, row);
            ImGui::Text("Biome: %s (temperature %.2f, humidity %.2f)", BiomeName(TerrainGenerator::Classify(c)), c.temperature, c.humidity);
        }
        const glm::vec3 velocity = player.GetVelocity();
        ImGui::Text("%s%s, velocity %.2f, %.2f, %.2f", player.IsFlying() ? "Flying" : player.IsSwimming() ? "Swimming" : "Walking",
                    player.IsFlying() || player.IsSwimming() ? "" : player.IsOnGround() ? " (on ground)" : " (airborne)",
                    velocity.x, velocity.y, velocity.z);
        ImGui::Text("Angles %.3f, %.3f", angles.x, angles.y);
        ImGui::Text("Frame %.3f ms (%.1f FPS)", deltaTime * 1000.0f, deltaTime > 0.0f ? 1.0f / deltaTime : 0.0f);
        ImGui::PlotLines("Frame Time (ms)", frameTimes.data(), kFrameHistory, frameTimeIndex);
        ImGui::Text("Chunks: %zu received, %zu tracked, %d alive", world->GetLoadedChunkCount(), world->GetChunkCount(), Chunk::GetLiveCount());
        const Client::Stats net = client->GetStats();
        ImGui::Text("Server: %s, player %u, ping %u ms", serverLabel.c_str(), client->GetPlayerId(), net.roundTripMs);
        ImGui::Text("Received %.1f MB: %u chunks, %u cell updates, %u light updates", net.bytesReceived / (1024.0 * 1024.0),
                    net.chunksReceived, net.deltasReceived, net.lightUpdates);
        ImGui::Text("Other players: %zu", client->GetRemotePlayers().size());

        const TextureAtlas& atlas = renderer->GetAtlas();
        if (ImGui::CollapsingHeader("Texture atlas")) {
            glm::ivec2 size = atlas.GetSize();
            ImGui::Text("%zu textures, %dx%d, %dpx tiles (R to reload)", atlas.GetNames().size() - 1, size.x, size.y, atlas.GetTileSize());
            const float scale = 128.0f / std::max(size.x, size.y);
            ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(atlas.GetTexture())), ImVec2(size.x * scale, size.y * scale));
        }

        const float previewScale = 0.25f;
        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(renderer->GetSceneTexture())),
                     ImVec2(renderer->GetWidth() * previewScale, renderer->GetHeight() * previewScale),
                     ImVec2(0, 1), ImVec2(1, 0));
    }
    ImGui::End();
}

void Application::OnKey(int key, int action) {
    if (action != GLFW_PRESS) return;

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

    // While typing, keys belong to the chat line (ImGui gets them too); Esc closes it.
    if (chatOpen) {
        if (key == GLFW_KEY_ESCAPE) CloseChat();
        return;
    }
    // The inventory takes I and Esc (to close) and the number keys (to pick a slot).
    if (inventoryOpen) {
        if ((key == GLFW_KEY_I && !signMode && !signalMode) || key == GLFW_KEY_ESCAPE) SetInventoryOpen(false); // (writing a sign, I is a letter)
        else if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) SelectSlot(key - GLFW_KEY_1);
        else if (key == GLFW_KEY_0) SelectSlot(9);
        return;
    }

    switch (key) {
    case GLFW_KEY_I:
        SetInventoryOpen(true);
        break;
    case GLFW_KEY_V: {
        // Placement height: full, 3/4, 1/2, 1/4 (Shift+V goes back up).
        const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        placeHeight = shift ? (placeHeight >= 8 ? 2 : placeHeight + 2) : (placeHeight <= 2 ? 8 : placeHeight - 2);
        break;
    }
    case GLFW_KEY_C:
        if (captureCursor) ChiselTarget(glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        break;
    case GLFW_KEY_T:
        OpenChat('t');
        break;
    case GLFW_KEY_SLASH:
        OpenChat('/');
        break;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
        OpenChat(0);
        break;
    case GLFW_KEY_ESCAPE:
        paused = true; // the pause menu: resume, settings, back to the title screen
        SetCursorCaptured(false);
        break;
    case GLFW_KEY_R:
        renderer->ReloadShaders();
        renderer->ReloadTextures();
        LoadBlockTextures();
        world->RebuildAllChunks();
        break;
    case GLFW_KEY_TAB:
        captureCursor = !captureCursor;
        glfwSetInputMode(window, GLFW_CURSOR, captureCursor ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        camera.ResetLook();
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
    case GLFW_KEY_M:
        showMap = !showMap;
        break;
    case GLFW_KEY_P:
        showPlayers = !showPlayers;
        break;
    case GLFW_KEY_F12:
        screenshotRequested = true;
        break;
    case GLFW_KEY_N:
        client->SkipTime();
        break;
    default:
        if (key >= GLFW_KEY_1 && key <= GLFW_KEY_9) SelectSlot(key - GLFW_KEY_1);
        else if (key == GLFW_KEY_0) SelectSlot(9);
        break;
    }
}

BlockType Application::SelectedBlock() const {
    return hotbar[selectedSlot];
}

void Application::SelectSlot(int slot) {
    selectedSlot = ((slot % kHotbarSlots) + kHotbarSlots) % kHotbarSlots;
}

void Application::LoadHotbar() {
    std::ifstream file(kHotbarFile);
    for (int i = 0, type; i < kHotbarSlots && file >> type; i++)
        if (std::find(std::begin(kInventoryBlocks), std::end(kInventoryBlocks), static_cast<BlockType>(type)) != std::end(kInventoryBlocks))
            hotbar[i] = static_cast<BlockType>(type);
    // A second line: the tool in each slot (0 none).
    for (int i = 0, tool; i < kHotbarSlots && file >> tool; i++)
        if (tool > 0 && tool < static_cast<int>(Tool::Count)) hotbarTools[i] = static_cast<Tool>(tool);
}

void Application::SaveHotbar() const {
    std::ofstream file(kHotbarFile, std::ios::trunc);
    for (BlockType type : hotbar) file << static_cast<int>(type) << ' ';
    file << "\n";
    for (Tool tool : hotbarTools) file << static_cast<int>(tool) << ' ';
    file << "\n";
}

void Application::SetInventoryOpen(bool open) {
    if (open == inventoryOpen) return;
    inventoryOpen = open;
    if (!open && chestMode) {
        client->CloseChest();
        chestMode = chestSeenOpen = false;
    }
    if (!open) signMode = false, signalMode = false;
    // The cursor is needed to click blocks; give it back to the game on closing.
    if (open) cursorBeforeInventory = captureCursor;
    captureCursor = open ? false : cursorBeforeInventory;
    glfwSetInputMode(window, GLFW_CURSOR, captureCursor ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    camera.ResetLook();
}

void Application::DrawInventory() {
    if (!inventoryOpen) return;
    if (chestMode) return DrawChest();
    if (signMode) return DrawSignEditor();
    if (signalMode) return DrawSignalEditor();
    constexpr int kColumns = 7;
    constexpr float kIcon = 44.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize;
    bool open = true;
    if (ImGui::Begin("Blocks", &open, flags)) {
        const char key = selectedSlot < 9 ? static_cast<char>('1' + selectedSlot) : '0';
        const bool survival = !client->IsCreative();
        ImGui::Text("Click a tool or block to put it in hotbar slot %c.", key);
        ImGui::TextDisabled("Pick the slot with 1-9, 0 or by clicking it below. I or Esc closes.");
        if (survival) ImGui::TextDisabled("Survival: these are the blocks you've collected. Break blocks to get more.");
        ImGui::Spacing();
        const TextureAtlas& atlas = renderer->GetAtlas();
        const ImTextureID texture = static_cast<ImTextureID>(static_cast<uintptr_t>(atlas.GetTexture()));
        // Tools first, a row of their own (free for now: there's no crafting yet).
        for (size_t t = 0; t < std::size(kInventoryTools); t++) {
            const Tool tool = kInventoryTools[t];
            if (t > 0) ImGui::SameLine();
            glm::vec2 uv0, uv1;
            atlas.GetTileUV(ToolTile(atlas, tool), uv0, uv1);
            ImGui::PushID(1000 + static_cast<int>(tool));
            const bool inSlot = hotbarTools[selectedSlot] == tool;
            if (inSlot) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
            if (ImGui::ImageButton("tool", texture, ImVec2(kIcon, kIcon), ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y))) {
                hotbarTools[selectedSlot] = tool;
                SaveHotbar();
            }
            if (inSlot) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                if (client->HasTool(tool)) ImGui::SetTooltip("%s: %s", Mining::ToolName(tool), ToolHint(tool));
                else ImGui::SetTooltip("%s: %s (not made yet: craft one below)", Mining::ToolName(tool), ToolHint(tool));
            }
            if (!client->HasTool(tool)) {
                // Not made yet: shaded over.
                ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(0, 0, 0, 150), 3.0f);
            }
            ImGui::PopID();
        }
        // Armour worn (survival): its pieces, shaded over until made, and what they
        // take off.
        if (survival) {
            ImGui::SameLine(0.0f, 24.0f);
            for (int p = 0; p < static_cast<int>(ArmourPiece::Count); p++) {
                const ArmourPiece piece = static_cast<ArmourPiece>(p);
                if (p > 0) ImGui::SameLine();
                glm::vec2 uv0, uv1;
                atlas.GetTileUV(atlas.GetIndex(Armour::Texture(piece)), uv0, uv1);
                ImGui::Image(texture, ImVec2(kIcon, kIcon), ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y));
                const bool worn = client->HasArmour(piece);
                if (!worn) ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(0, 0, 0, 150), 3.0f);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s: %d%% less damage from hits and traps%s", Armour::Name(piece),
                                      static_cast<int>(std::lround(Armour::Protection(piece) * 100.0f)), worn ? " (worn)" : " (not made yet: craft it from hide below)");
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Armour: %d%%", static_cast<int>(std::lround(Armour::Protection(client->GetArmour()) * 100.0f)));
        }
        ImGui::Separator();
        int column = 0;
        for (BlockType type : kInventoryBlocks) {
            const uint32_t held = client->GetResources(type);
            // Survival: only what you have (and not what's counted as something else:
            // pebbles are stone, coal ore is coal).
            if (survival && (held == 0 || ResourceTypeOf(type) != type)) continue;
            if (column++ % kColumns != 0) ImGui::SameLine();
            const ImVec2 corner = ImGui::GetCursorScreenPos();
            glm::vec2 uv0, uv1;
            atlas.GetTileUV(IconTile(type), uv0, uv1);
            ImGui::PushID(static_cast<int>(type));
            const bool inSlot = hotbarTools[selectedSlot] == Tool::None && hotbar[selectedSlot] == type;
            if (inSlot) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
            if (ImGui::ImageButton("block", texture, ImVec2(kIcon, kIcon), ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y))) {
                hotbar[selectedSlot] = type;
                hotbarTools[selectedSlot] = Tool::None;
                SaveHotbar();
            }
            if (inSlot) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                if (survival) ImGui::SetTooltip("%s: %s", BlockName(type), FormatBlocks(held).c_str());
                else ImGui::SetTooltip("%s", BlockName(type));
            }
            if (survival) {
                const std::string count = FormatBlocks(held);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImVec2 size = ImGui::CalcTextSize(count.c_str());
                const ImVec2 at(corner.x + kIcon + 4.0f - size.x, corner.y + kIcon + 4.0f - size.y);
                draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 230), count.c_str());
                draw->AddText(at, IM_COL32_WHITE, count.c_str());
            }
            ImGui::PopID();
        }
        if (survival && column == 0) ImGui::TextUnformatted("Nothing yet: break some blocks.");

        // Crafting (survival): what each recipe takes, and buttons to make it.
        if (survival) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Crafting");
            const auto& recipes = Crafting::Recipes();
            // In a scrolling box, so the window stays on screen.
            ImGui::BeginChild("crafting", ImVec2(0.0f, 260.0f), true);
            if (ImGui::BeginTable("recipes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
                for (int r = 0; r < static_cast<int>(recipes.size()); r++) {
                    const Recipe& recipe = recipes[r];
                    ImGui::PushID(r);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    glm::vec2 uv0, uv1;
                    const int icon = recipe.ironUpgrade                     ? atlas.GetIndex("iron_ingot")
                                     : recipe.tool != Tool::None              ? ToolTile(atlas, recipe.tool)
                                     : recipe.armour != ArmourPiece::Count ? atlas.GetIndex(Armour::Texture(recipe.armour))
                                                                           : IconTile(recipe.output);
                    atlas.GetTileUV(icon, uv0, uv1);
                    ImGui::Image(texture, ImVec2(24.0f, 24.0f), ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y));
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(recipe.name);
                    ImGui::TableNextColumn();
                    std::string needs;
                    for (const auto& in : recipe.inputs)
                        needs += (needs.empty() ? "" : " + ") + FormatBlocks(in.units) + " " + BlockName(in.type);
                    const bool owned = (recipe.ironUpgrade && client->HasIron()) || (recipe.tool != Tool::None && client->HasTool(recipe.tool)) ||
                                       (recipe.armour != ArmourPiece::Count && client->HasArmour(recipe.armour));
                    if (owned) ImGui::TextDisabled(recipe.armour != ArmourPiece::Count ? "worn" : "made");
                    else if (client->CanCraft(r)) ImGui::TextUnformatted(needs.c_str());
                    else ImGui::TextDisabled("%s", needs.c_str());
                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(!client->CanCraft(r));
                    if (ImGui::SmallButton("Make")) {
                        client->Craft(r, 1);
                        audio->PlayHere("craft", 0.6f);
                    }
                    ImGui::EndDisabled();
                    if (!recipe.MadeOnce()) {
                        ImGui::SameLine();
                        ImGui::BeginDisabled(!client->CanCraft(r, 4));
                        if (ImGui::SmallButton("x4")) {
                            client->Craft(r, 4);
                            audio->PlayHere("craft", 0.6f);
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
    if (!open) SetInventoryOpen(false);
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
    constexpr int kColumns = 8;
    constexpr float kIcon = 40.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize;
    bool keepOpen = true;
    if (ImGui::Begin("Chest", &keepOpen, flags)) {
        const TextureAtlas& atlas = renderer->GetAtlas();
        const ImTextureID texture = static_cast<ImTextureID>(static_cast<uintptr_t>(atlas.GetTexture()));
        const bool all = ImGui::GetIO().KeyShift;
        ImGui::TextDisabled("Click to move a block's worth (or one item); Shift-click moves them all. I or Esc closes.");
        // One grid: every type with some in it, each a button that moves some across.
        auto grid = [&](const char* id, bool inChest) {
            int column = 0;
            for (int t = 1; t < static_cast<int>(BlockType::COUNT); t++) {
                const BlockType type = static_cast<BlockType>(t);
                const uint32_t units = inChest ? client->GetChestUnits(type) : client->GetResources(type);
                if (units == 0) continue;
                if (column++ % kColumns != 0) ImGui::SameLine();
                const ImVec2 corner = ImGui::GetCursorScreenPos();
                glm::vec2 uv0, uv1;
                atlas.GetTileUV(IconTile(type), uv0, uv1);
                ImGui::PushID(t + (inChest ? 1000 : 0));
                if (ImGui::ImageButton(id, texture, ImVec2(kIcon, kIcon), ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y))) {
                    client->MoveChestItems(type, all ? units : std::min<uint32_t>(units, Block::kUnitsPerBlock), !inChest);
                    audio->PlayHere("pickup", 0.4f);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", BlockName(type), FormatBlocks(units).c_str());
                const std::string count = FormatBlocks(units);
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImVec2 size = ImGui::CalcTextSize(count.c_str());
                const ImVec2 at(corner.x + kIcon + 4.0f - size.x, corner.y + kIcon + 4.0f - size.y);
                draw->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 230), count.c_str());
                draw->AddText(at, IM_COL32_WHITE, count.c_str());
                ImGui::PopID();
            }
            if (column == 0) ImGui::TextDisabled(inChest ? "Empty." : "Nothing to put in.");
        };
        ImGui::SeparatorText("In the chest (click to take)");
        grid("chest", true);
        ImGui::SeparatorText("Yours (click to put in)");
        grid("mine", false);
    }
    ImGui::End();
    if (!keepOpen) SetInventoryOpen(false);
}

void Application::ChiselTarget(bool raise) {
    auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), Reach());
    if (!hit) return;
    // Where the view ray enters the block's cube decides the quarter (in x and z).
    const glm::vec3 origin = camera.GetPosition(), dir = camera.GetDirection();
    const glm::vec3 lo(hit->block), hi = lo + glm::vec3(1.0f);
    float tNear = 0.0f, tFar = 1e9f;
    for (int a = 0; a < 3; a++) {
        if (std::abs(dir[a]) < 1e-6f) continue;
        float t0 = (lo[a] - origin[a]) / dir[a], t1 = (hi[a] - origin[a]) / dir[a];
        if (t0 > t1) std::swap(t0, t1);
        tNear = std::max(tNear, t0);
        tFar = std::min(tFar, t1);
    }
    const glm::vec3 p = origin + dir * std::min(tNear, tFar) - lo;
    const bool highX = p.x >= 0.5f, highZ = p.z >= 0.5f;
    const int edge = highX ? (highZ ? 3 : 1) : (highZ ? 0 : 2); // EdgeData order
    client->Chisel(hit->block, edge, raise);
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
            BlockType type = ResourceTypeOf(looked.type) == looked.type || IsItemType(ResourceTypeOf(looked.type)) ? looked.type : ResourceTypeOf(looked.type);
            if (type == BlockType::DOOR_OPEN) type = BlockType::DOOR;
            if (type == BlockType::TNT_LIT) type = BlockType::TNT;
            if (!IsItemType(type) && (client->IsCreative() || client->GetResources(type) > 0)) {
                hotbar[selectedSlot] = type;
                hotbarTools[selectedSlot] = Tool::None;
                SaveHotbar();
            }
        }
        return;
    }

    // Right-click on a creature: feed it (raw venison tames a wild wolf).
    if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        const FightTarget target = TargetInView();
        if (target.creature) {
            client->Interact(target.creature);
            return;
        }
    }

    // Right-click on a chest within 6 blocks opens it (with Shift held, or further off,
    // it's built against instead).
    if (button == GLFW_MOUSE_BUTTON_RIGHT && glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) != GLFW_PRESS) {
        Block target;
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
            at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && target.type == BlockType::CHEST) {
            client->OpenChest(at->block);
            SetInventoryOpen(true);
            chestMode = true;
            chestSeenOpen = false;
            chestAskedAt = Client::Now();
            return;
        }
        // With a block in hand, right-clicking machinery builds against it (a wheel on a
        // shaft, a pipe on a compressor); with an empty hand or a tool, it's used.
        const bool building = SelectedTool() == Tool::None && SelectedBlock() != BlockType::AIR && !IsItemType(SelectedBlock());
        // A signal block: crouching, set its channels; a gate or a lamp (nothing else to
        // do with them), likewise when not building.
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
            at && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && Signals::KindOf(target.type) != Signals::Kind::None &&
            (player.IsCrouching() ||
             (!building && (target.type == BlockType::LOGIC_GATE || target.type == BlockType::SIGNAL_LAMP || target.type == BlockType::SIGNAL_LAMP_ON)))) {
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
             (!building && (target.type == BlockType::VALVE || target.type == BlockType::VALVE_OPEN)))) {
            client->UseBed(at->block);
            return;
        }
        // Part of an air network: how it's doing (the server says, in chat).
        if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), 6.0f);
            at && !building && world->GetBlock(at->block.x, at->block.y, at->block.z, target) && Power::IsPart(target.type)) {
            client->Inspect(at->block);
            return;
        }
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

    // Right-click with the bow drawn: shoot (if there's an arrow, in survival).
    if (button == GLFW_MOUSE_BUTTON_RIGHT && SelectedTool() == Tool::Bow) {
        if (client->HasTool(Tool::Bow) && (client->IsCreative() || client->GetResources(BlockType::ARROW) >= Block::kUnitsPerBlock) &&
            Client::Now() >= nextShot) {
            client->Shoot(camera.GetDirection());
            audio->PlayHere("bow", 0.7f);
            nextShot = Client::Now() + 0.6;
        }
        return;
    }

    // Right-click with an item in hand uses it (wherever you look): food is eaten;
    // items are never placed.
    if (button == GLFW_MOUSE_BUTTON_RIGHT && SelectedTool() == Tool::None && IsItemType(SelectedBlock())) {
        const BlockType item = SelectedBlock();
        // A boat: into it, on the water looked at (within 4 blocks).
        if (item == BlockType::BOAT) {
            const glm::vec3 eye = camera.GetPosition(), dir = camera.GetDirection();
            for (float t = 0.5f; t < 4.0f && !player.IsBoating(); t += 0.1f) {
                const glm::ivec3 cell(glm::floor(eye + dir * t));
                Block b, above;
                if (!world->GetBlock(cell.x, cell.y, cell.z, b)) break;
                if (b.HasWater() && !(world->GetBlock(cell.x, cell.y + 1, cell.z, above) && above.HasWater())) {
                    if (client->IsCreative() || client->GetResources(BlockType::BOAT) >= Block::kUnitsPerBlock) {
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
        if (item == BlockType::SEEDS) {
            Block soil;
            if (auto at = Raycast(*world, camera.GetPosition(), camera.GetDirection(), Reach());
                at && at->normal == glm::ivec3(0, 1, 0) && world->GetBlock(at->block.x, at->block.y, at->block.z, soil) &&
                (soil.type == BlockType::DIRT || IsGrassType(soil.type)) && client->CanAfford(BlockType::CROP, 8)) {
                client->PlaceBlock(at->block + at->normal, BlockType::CROP);
                PlayAt("place", at->block + at->normal, 0.5f);
            }
            return;
        }
        if (Hunger::IsFood(item) && !client->IsCreative() && client->GetResources(item) >= Block::kUnitsPerBlock &&
            client->GetFood() < client->GetMaxFood()) {
            client->Eat(item);
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
        const glm::ivec3 place = hit->block + hit->normal;
        // Don't build a solid block inside yourself when walking (water, sprites and
        // pressure plates are fine).
        const BlockType placeBlock = SelectedBlock();
        const int height = Block::CanShape(placeBlock) ? placeHeight : 8;
        if (!client->CanAfford(placeBlock, height)) return;
        const Block placed = Block::Placed(placeBlock, height);
        if (!player.IsFlying() && (placed.IsFullBlock() || Block::CanShape(placeBlock))) {
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
        client->PlaceBlock(place, placeBlock, height, faceAxis);
        PlayAt("place", place, 0.8f);
        if (placeBlock == BlockType::SIGN) OpenSign(place); // and write on it
        // Signal blocks: set their channels straight away.
        if (placeBlock == BlockType::LEVER || placeBlock == BlockType::LOGIC_GATE || placeBlock == BlockType::SIGNAL_LAMP) OpenSignalEditor(place, placeBlock);
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
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.4f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    const char* titles[] = { "Signal", "Lever", "Pressure plate", "Logic gate", "Signal lamp", "Valve", "Sluice gate" };
    bool open = true;
    if (ImGui::Begin(titles[std::clamp(signalKind, 0, 6)], &open,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        auto channel = [](const char* label, int& value) {
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputInt(label, &value)) value = std::clamp(value, 0, 255);
        };
        ImGui::TextDisabled("Channels 1-255 (0: none). Signals reach %.0f blocks.", Signals::kRange);
        if (kind == Signals::Kind::Gate) {
            const char* modes[] = { "AND (on while A and B are)", "OR (on while A or B is)", "XOR (on while just one of A, B is)", "NOT (on while A isn't)",
                                    "RELAY (sends A on again, further)", "CLOCK (on and off each second while A is on, or always)" };
            ImGui::SetNextItemWidth(360.0f);
            ImGui::Combo("Does", &signalModeIndex, modes, IM_ARRAYSIZE(modes));
            channel("Input A", signalA);
            if (Signals::Inputs(static_cast<Signals::Mode>(signalModeIndex)) == 2) channel("Input B", signalB);
            channel("Sends on", signalOut);
        }
        else if (Signals::Sends(kind)) {
            channel("Sends on", signalOut);
            ImGui::TextDisabled(kind == Signals::Kind::Lever ? "Right-click the lever to flip it." : "It sends while stood on, and for a second after.");
        }
        else {
            channel("Listens to", signalA);
            if (kind == Signals::Kind::Lamp) ImGui::TextDisabled("Lights while the channel is on.");
            if (kind == Signals::Kind::Valve) ImGui::TextDisabled("Opens while the channel is on. With none, right-click (empty-handed) opens and shuts it.");
            if (kind == Signals::Kind::Sluice) ImGui::TextDisabled("Opens while the channel is on, and closes again when it's off.");
        }
        if (ImGui::Button("Done") || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
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
        ImGui::SameLine();
        ImGui::TextDisabled("Crouch and right-click to change it later.");
    }
    ImGui::End();
    if (!open) SetInventoryOpen(false);
}

void Application::DrawSignEditor() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f, viewport->WorkPos.y + viewport->WorkSize.y * 0.4f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    bool open = true;
    if (ImGui::Begin("Sign", &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("What's written on it (Enter to finish, Esc to leave it):");
        ImGui::SetNextItemWidth(420.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##sign", signText, net::kMaxSignLength + 1, ImGuiInputTextFlags_EnterReturnsTrue)) {
            client->WriteSign(signAt, signText);
            open = false;
        }
    }
    ImGui::End();
    if (!open) SetInventoryOpen(false);
}

void Application::DrawSigns(const glm::mat4& viewProjection) {
    // Each sign's words over it, within 12 blocks and in sight, like a little plaque.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
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
        const ImVec2 c(viewport->WorkPos.x + (ndc.x * 0.5f + 0.5f) * viewport->WorkSize.x, viewport->WorkPos.y + (0.5f - ndc.y * 0.5f) * viewport->WorkSize.y);
        const float fade = std::clamp((12.0f - distance) / 3.0f, 0.0f, 1.0f);
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 lo(c.x - size.x * 0.5f - 7.0f, c.y - size.y - 6.0f), hi(c.x + size.x * 0.5f + 7.0f, c.y + 2.0f);
        draw->AddRectFilled(lo, hi, IM_COL32(196, 160, 104, static_cast<int>(225 * fade)), 2.0f);
        draw->AddRect(lo, hi, IM_COL32(110, 76, 38, static_cast<int>(255 * fade)), 2.0f, 0, 2.0f);
        draw->AddText(ImVec2(lo.x + 7.0f, lo.y + 4.0f), IM_COL32(40, 26, 12, static_cast<int>(255 * fade)), text.c_str());
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
        const float t = enters(p.feet - glm::vec3(0.3f, 0.0f, 0.3f), p.feet + glm::vec3(0.3f, 1.8f, 0.3f));
        if (t >= 0.0f && t < bestT) bestT = t, best = FightTarget{ 0, p.id };
    }
    return best;
}

void Application::UpdateMining() {
    mineProgress = 0.0f;
    const bool held = (captureCursor && !chatOpen && !inventoryOpen && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) || holdMine;
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
    const double needed = Mining::Seconds(block, tool, client->HasIron() && !client->IsCreative()), elapsed = now - miningStart;
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
        const glm::ivec3 c(glm::floor(player.GetFeetPosition()));
        for (int x = -16; x <= 16; x++)
            for (int z = -16; z <= 16; z++)
                for (int y = -6; y <= 6; y++) {
                    Block b;
                    if (world->GetBlock(c.x + x, c.y + y, c.z + z, b) && b.type == BlockType::CAMPFIRE) campfires.push_back(c + glm::ivec3(x, y, z));
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
        if (p.rise) {
            p.velocity.y += (1.1f - p.velocity.y) * std::min(1.0f, dt * 2.0f);
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

    // Creatures: a thump when hit, and now and then a ghoul's groan.
    std::uniform_real_distribution<double> wait(4.0, 11.0);
    static std::mt19937 rng(99);
    for (auto& [id, c] : creatureSounds) c.seen = false;
    for (const auto& c : client->GetCreatures()) {
        CreatureSound& s = creatureSounds[c.id];
        if (c.hurt && !s.hurt) audio->Play("hit", c.feet + glm::vec3(0.0f, 0.8f, 0.0f), 0.9f);
        s.hurt = c.hurt;
        if (c.type == CreatureType::Ghoul) {
            if (s.nextGroan == 0.0) s.nextGroan = now + wait(rng) * 0.5;
            else if (now >= s.nextGroan) {
                audio->Play("groan", c.feet + glm::vec3(0.0f, 1.6f, 0.0f), 0.8f, 0.15f);
                s.nextGroan = now + wait(rng);
            }
        }
        s.seen = true;
    }
    for (auto it = creatureSounds.begin(); it != creatureSounds.end();) it = it->second.seen ? std::next(it) : creatureSounds.erase(it);

    // Rain, as heard here (none under a roof).
    audio->SetLoop("rain_loop", rainHere * 0.45f);
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
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnScroll(yOffset);
}

void Application::KeyCallback(GLFWwindow* window, int key, int, int action, int) {
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnKey(key, action);
}

void Application::MouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnMouseButton(button, action);
}

void Application::CursorPosCallback(GLFWwindow* window, double x, double y) {
    static_cast<Application*>(glfwGetWindowUserPointer(window))->OnCursorMove(x, y);
}

float Application::Reach() const {
    return client && client->IsCreative() ? kCreativeReach : kSurvivalReach;
}
