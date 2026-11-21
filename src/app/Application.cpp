#include "app/Application.h"

#include <algorithm>
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
#include "world/Breathing.h"
#include "world/Chunk.h"
#include "world/Crafting.h"
#include "world/Raycast.h"
#include "world/TerrainGenerator.h"

namespace {

constexpr int kWindowWidth = 1200;
constexpr int kWindowHeight = 700;
constexpr const char* kGlslVersion = "#version 330 core";

constexpr float kReach = 50.0f;
constexpr float kLaserRange = 100.0f;
constexpr int kLaserRadius = 4;
// Everything the inventory (I) offers, in the order it shows them.
constexpr BlockType kInventoryBlocks[] = {
    BlockType::DIRT, BlockType::GRASS, BlockType::DRY_GRASS, BlockType::LUSH_GRASS, BlockType::SNOWY_GRASS, BlockType::STONE, BlockType::SAND,
    BlockType::SNOW, BlockType::ICE, BlockType::LOG, BlockType::LEAVES, BlockType::PINE_LEAVES, BlockType::CACTUS, BlockType::LAMP,
    BlockType::STONE_BRICK, BlockType::MOSSY_BRICK, BlockType::CRUMBLING_BRICK, BlockType::WATER, BlockType::PUMP, BlockType::PIPE,
    BlockType::SLUICE, BlockType::PRESSURE_PLATE, BlockType::SPIKES, BlockType::TALL_GRASS, BlockType::FLOWER_RED, BlockType::FLOWER_YELLOW,
    BlockType::DEAD_BUSH, BlockType::PEBBLES, BlockType::PLANKS, BlockType::GLASS,
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
constexpr Tool kInventoryTools[] = { Tool::Pickaxe, Tool::Axe, Tool::Shovel };
int ToolTile(const TextureAtlas& atlas, Tool tool) {
    const char* name = Mining::ToolTexture(tool);
    return name ? atlas.GetIndex(name) : 0;
}
const char* ToolHint(Tool tool) {
    switch (tool) {
    case Tool::Pickaxe: return "fast on stone, bricks, ice and machines";
    case Tool::Axe: return "fast on wood";
    case Tool::Shovel: return "fast on dirt, grass, sand and snow";
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
        HandleContinuousInput(deltaTime);
        camera.SetPosition(player.GetEyePosition());
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
        if (auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), kReach)) {
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
            return any ? best : glm::vec2(15.0f, 0.0f);
        };
        for (const auto& c : client->GetCreatures())
            params.creatures.push_back({ static_cast<uint8_t>(c.type), c.feet, c.yaw, c.hurt, c.speed, lightAt(c.feet) });
        for (const auto& other : client->GetRemotePlayers()) {
            params.players.push_back({ other.feet, other.yaw, other.pitch, PlayerModels::ColorFor(other.id), (other.flags & net::kFlagHurt) != 0, lightAt(other.feet) });
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
    const float target = (sheltered || params.underwater) ? 0.0f : rain;
    const float ease = std::min(1.0f, deltaTime * 4.0f);
    rainHere += (target - rainHere) * ease;
    params.rain = rainHere;
    params.overcast = rain;
    params.sunBrightness = day * (1.0f - 0.45f * rain);
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

    // Air bubbles above the hotbar's right end, right to left, while anything takes
    // your breath or it's coming back; a bubble is two units of air.
    if (cause != Breathing::Cause::None || air < maxAir) {
        const int bubbles = (maxAir + 1) / 2;
        const float right = viewport->WorkPos.x + (viewport->WorkSize.x + hotbarWidth) * 0.5f - 6.0f;
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
        client->SendPlayerState(player.GetFeetPosition(), player.GetVelocity(), camera.GetYaw(), camera.GetDirectionAngles().x, flags);
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
        if (key == GLFW_KEY_I || key == GLFW_KEY_ESCAPE) SetInventoryOpen(false);
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
    // The cursor is needed to click blocks; give it back to the game on closing.
    if (open) cursorBeforeInventory = captureCursor;
    captureCursor = open ? false : cursorBeforeInventory;
    glfwSetInputMode(window, GLFW_CURSOR, captureCursor ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    camera.ResetLook();
}

void Application::DrawInventory() {
    if (!inventoryOpen) return;
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
        ImGui::Separator();
        int column = 0;
        for (BlockType type : kInventoryBlocks) {
            const uint32_t held = client->GetResources(type);
            if (survival && (held == 0 || type == BlockType::PEBBLES)) continue; // survival: only what you have (pebbles are stone)
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
                    atlas.GetTileUV(recipe.tool != Tool::None ? ToolTile(atlas, recipe.tool) : IconTile(recipe.output), uv0, uv1);
                    ImGui::Image(texture, ImVec2(24.0f, 24.0f), ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y));
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(recipe.name);
                    ImGui::TableNextColumn();
                    std::string needs;
                    for (const auto& in : recipe.inputs)
                        needs += (needs.empty() ? "" : " + ") + FormatBlocks(in.units) + " " + BlockName(in.type);
                    const bool owned = recipe.tool != Tool::None && client->HasTool(recipe.tool);
                    if (owned) ImGui::TextDisabled("made");
                    else if (client->CanCraft(r)) ImGui::TextUnformatted(needs.c_str());
                    else ImGui::TextDisabled("%s", needs.c_str());
                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(!client->CanCraft(r));
                    if (ImGui::SmallButton("Make")) {
                        client->Craft(r, 1);
                        audio->PlayHere("craft", 0.6f);
                    }
                    ImGui::EndDisabled();
                    if (recipe.tool == Tool::None) {
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

void Application::ChiselTarget(bool raise) {
    auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), kReach);
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

    auto hit = Raycast(*world, camera.GetPosition(), camera.GetDirection(), kReach);
    if (!hit) return;

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        // Creative breaks at once; survival mines while the button is held (UpdateMining).
        if (client->IsCreative()) {
            Block broken;
            if (world->GetBlock(hit->block.x, hit->block.y, hit->block.z, broken)) PlayAt(BreakSound(broken.type), hit->block);
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
        // Logs lie along the axis of the face they're placed against.
        const int faceAxis = hit->normal.x != 0 ? 0 : hit->normal.z != 0 ? 2 : 1;
        client->PlaceBlock(place, placeBlock, height, faceAxis);
        PlayAt("place", place, 0.8f);
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
                   ? Raycast(*world, camera.GetPosition(), camera.GetDirection(), kReach)
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
    const double needed = Mining::Seconds(block, tool), elapsed = now - miningStart;
    mineProgress = needed > 0.0 ? static_cast<float>(std::min(1.0, elapsed / needed)) : 1.0f;
    // A tap on the block every so often while mining it.
    if (needed > 0.0 && now >= nextDig) {
        const std::string material = Audio::Material(block.type);
        PlayAt(material == "hard" || material == "glass" ? "dig_hard" : "dig_soft", miningPos, 0.6f);
        nextDig = now + 0.27;
    }
    if (elapsed >= needed) {
        PlayAt(BreakSound(block.type), miningPos);
        client->BreakBlock(miningPos, tool);
        mining = false;
        mineProgress = 0.0f;
        nextMineAt = now + 0.15; // a moment before starting on the next block
    }
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
    if (player.IsSwimming() && !wasSwimming) audio->Play("splash", feet, 0.6f);
    wasSwimming = player.IsSwimming();
    // Hurt.
    if (client->GetLastHurt() > lastHurtHeard + 0.05) {
        lastHurtHeard = client->GetLastHurt();
        audio->PlayHere("hurt", 0.8f);
    }

    // What others broke and built (a few at a time: a collapse or the laser makes many).
    int played = 0;
    for (const auto& event : client->TakeBlockEvents())
        if (played++ < 4) PlayAt(event.placed ? "place" : BreakSound(event.type), event.pos, 0.8f);

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
