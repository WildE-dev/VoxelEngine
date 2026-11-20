#pragma once

#include <array>
#include <memory>

#include "app/Camera.h"
#include "app/Player.h"
#include "world/Block.h"
#include "world/TerrainGenerator.h"

struct GLFWwindow;
class Renderer;
class World;

class Application
{
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool Init();
    void Run();

private:
    void Shutdown();
    void LoadBlockTextures();
    void DrawHotbar();
    BlockType SelectedBlock() const;
    void SelectSlot(int slot);

    void HandleContinuousInput(float deltaTime);
    void DrawDebugWindow(float deltaTime);

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
    TerrainGenerator terrain;
    // Both own GL resources, so they're destroyed explicitly before the context.
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<World> world;

    bool captureCursor = true;
    bool wireframe = false;
    bool ambientOcclusion = true;
    float sunBrightness = 1.0f;
    int selectedSlot = 0; // index into the hotbar (kPlaceableBlocks)
    double scrollAccumulator = 0.0;
    bool showDebug = false;
    bool vsync = true;

    static constexpr int kFrameHistory = 100;
    std::array<float, kFrameHistory> frameTimes{};
    int frameTimeIndex = 0;
    float frameSampleTimer = 0.0f;
};
