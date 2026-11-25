#pragma once
// The game's own user interface: an immediate-mode UI (each frame the code that shows
// a window or a button says so, and learns whether it was clicked), drawn with one
// shader as batches of textured quads over the 3D view.
//
//  - Drawing: DrawLists of rectangles, lines, circles, triangles, images and text, each
//    clipped to a rectangle. Text and solid shapes share one texture (the font atlas,
//    which has a white square in its corner); images use their own (the block atlas).
//  - Text: the UI font (Resources/Fonts/ui.png and ui.json, baked by
//    tools/bake_ui_font.py), scaled to any size.
//  - Input: the game passes in the mouse, the keys and the characters typed (and a
//    gamepad), and reads back whether the UI wants them (typing into a box, the mouse
//    over a window).
//  - Windows: placed by the game (where, how big or sized to what's in them), drawn
//    in the order they're shown, with a title bar and a close button if wanted. Inside,
//    items are laid out top to bottom (SameLine puts the next beside the last).
//  - Widgets: text, buttons, checkboxes, sliders, number and text boxes, drop-down
//    lists, images, scrolling areas, tables, collapsing sections, tooltips, a modal
//    popup and a line graph. Each has an id from its label (text after "##" isn't
//    shown, text after "###" is the whole id) within the window's (and PushID's).
//  - A gamepad's D-pad moves between the buttons and boxes, A presses, left and right
//    move sliders.
// The call shapes follow the parts of Dear ImGui the game used before, so screens
// could move across as they were.
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct Vec2 {
    float x = 0.0f, y = 0.0f;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
};
inline Vec2 operator+(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
inline Vec2 operator*(Vec2 a, float s) { return { a.x * s, a.y * s }; }
struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};
// Colours are 0xAABBGGRR (red in the low byte).
using Color = uint32_t;
constexpr Color Rgba(int r, int g, int b, int a = 255) {
    return static_cast<Color>(r & 255) | (static_cast<Color>(g & 255) << 8) | (static_cast<Color>(b & 255) << 16) | (static_cast<Color>(a & 255) << 24);
}
Color ToColor(const Vec4& c);
Vec4 ToVec4(Color c);
// A GL texture to draw an image from (0: the UI's own atlas).
using TextureId = unsigned int;

struct Font;

// What one draw call draws: a texture, a clip rectangle, and a run of indices.
struct DrawCmd {
    TextureId texture = 0;
    Vec4 clip; // min x, min y, max x, max y (screen)
    uint32_t firstIndex = 0, indexCount = 0;
};
struct Vertex {
    Vec2 pos, uv;
    Color color = 0;
};

class DrawList {
public:
    void AddRectFilled(Vec2 a, Vec2 b, Color c, float rounding = 0.0f, int flags = 0);
    void AddRect(Vec2 a, Vec2 b, Color c, float rounding = 0.0f, int flags = 0, float thickness = 1.0f);
    void AddRectFilledMultiColor(Vec2 a, Vec2 b, Color topLeft, Color topRight, Color bottomRight, Color bottomLeft);
    void AddLine(Vec2 a, Vec2 b, Color c, float thickness = 1.0f);
    void AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, Color col);
    void AddQuadFilled(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color col);
    void AddCircleFilled(Vec2 centre, float radius, Color col, int segments = 0);
    void AddCircle(Vec2 centre, float radius, Color col, int segments = 0, float thickness = 1.0f);
    void AddPolyline(const Vec2* points, int count, Color col, float thickness = 1.0f);
    void AddImage(TextureId texture, Vec2 a, Vec2 b, Vec2 uv0 = { 0, 0 }, Vec2 uv1 = { 1, 1 }, Color col = 0xFFFFFFFFu);
    void AddText(Vec2 pos, Color col, const char* text, const char* end = nullptr);
    void AddText(const Font* font, float size, Vec2 pos, Color col, const char* text, const char* end = nullptr, float wrapWidth = 0.0f);
    void PushClipRect(Vec2 a, Vec2 b, bool intersect = true);
    void PopClipRect();

    // (For the renderer.)
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<DrawCmd> commands;
    void Clear();

private:
    std::vector<Vec4> clips;
    Vec4 Clip() const;
    void Begin(TextureId texture);
    void Quad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Vec2 uva, Vec2 uvb, Vec2 uvc, Vec2 uvd, Color ca, Color cb, Color cc, Color cd);
    void Fan(const std::vector<Vec2>& points, Color col);
};

// A font: the glyphs of the baked atlas, and how big it's drawn.
struct Glyph {
    float x = 0, y = 0, w = 0, h = 0, xoff = 0, yoff = 0, advance = 0;
};
struct Font {
    float bakedSize = 32.0f, ascent = 32.0f, descent = 8.0f, atlasSize = 512.0f;
    Vec2 white{ 1.5f, 1.5f };
    Glyph glyphs[128];
    Glyph missing;
    const Glyph& Get(unsigned c) const { return c >= 32 && c < 127 ? glyphs[c] : missing; }
    // The height of a line of text at `size`.
    float LineHeight(float size) const { return (ascent + descent) * size / bakedSize; }
    // How wide and tall a run of text is at `size` (wrapped at `wrapWidth`, if above 0).
    Vec2 Measure(float size, const char* text, const char* end = nullptr, float wrapWidth = 0.0f) const;
};

// --- Setup, input, frames ----------------------------------------------------------

struct Viewport {
    Vec2 Pos, Size, WorkPos, WorkSize;
};
enum ConfigFlags : int { ConfigFlags_None = 0, ConfigFlags_NoMouse = 1, ConfigFlags_NoKeyboard = 2 };
enum class Key { None, Enter, Escape, Backspace, Delete, Left, Right, Up, Down, Home, End, Tab, UpArrow = Up, DownArrow = Down };
enum MouseButton : int { MouseButton_Left = 0, MouseButton_Right = 1, MouseButton_Middle = 2 };
struct IO {
    Vec2 MousePos{ -1e6f, -1e6f };
    bool MouseDown[3] = {};
    float MouseWheel = 0.0f;
    int ConfigFlags = 0;
    Vec2 DisplaySize;
    bool WantCaptureMouse = false;    // the mouse is over a window (the game shouldn't use it)
    bool WantTextInput = false;       // a text box has the keyboard
    float FontSize = 14.0f;           // the UI's text size, in screen pixels
};

// Loads the font (false, with `error`, if it can't). Call once a GL context exists.
bool Init(const std::string& fontDirectory, std::string& error);
void Shutdown();
IO& GetIO();
// Input, from the window's callbacks.
void AddKey(Key key, bool ctrl = false);
void AddChar(unsigned codepoint);
void AddMouseButton(int button, bool down);
void AddMouseWheel(float delta);
void SetMousePos(Vec2 pos);
// A gamepad this frame: its D-pad and A and B (true while held).
void SetGamepad(bool up, bool down, bool left, bool right, bool a, bool b);
// Where paste comes from (the window's clipboard).
void SetClipboardGetter(const char* (*get)(void* user), void* user);

void NewFrame(Vec2 displaySize, float deltaTime);
// Ends the frame and draws it into the current framebuffer (`framebufferScale`: its
// pixels per display pixel).
void Render(Vec2 framebufferSize);

const Viewport* GetMainViewport();
DrawList* GetForegroundDrawList();
DrawList* GetBackgroundDrawList();
DrawList* GetWindowDrawList();
const Font* GetFont();
float GetFontSize();

// --- Windows -----------------------------------------------------------------------

enum WindowFlags : int {
    WindowFlags_None = 0,
    WindowFlags_NoTitleBar = 1 << 0,
    WindowFlags_NoResize = 1 << 1,
    WindowFlags_NoMove = 1 << 2,
    WindowFlags_NoCollapse = 1 << 3,
    WindowFlags_AlwaysAutoResize = 1 << 4,
    WindowFlags_NoSavedSettings = 1 << 5,
    WindowFlags_NoInputs = 1 << 6,
    WindowFlags_NoFocusOnAppearing = 1 << 7,
    WindowFlags_NoNav = 1 << 8,
    WindowFlags_NoBringToFrontOnFocus = 1 << 9,
    WindowFlags_NoBackground = 1 << 10,
    WindowFlags_NoScrollbar = 1 << 11,
    WindowFlags_NoDecoration = WindowFlags_NoTitleBar | WindowFlags_NoResize | WindowFlags_NoCollapse | WindowFlags_NoScrollbar,
};
enum Cond : int { Cond_Always = 0, Cond_Once = 1, Cond_FirstUseEver = 2, Cond_Appearing = 3 };

void SetNextWindowPos(Vec2 pos, int cond = Cond_Always, Vec2 pivot = { 0, 0 });
void SetNextWindowSize(Vec2 size, int cond = Cond_Always);
void SetNextWindowSizeConstraints(Vec2 min, Vec2 max);
void SetNextWindowBgAlpha(float alpha);
// Shows a window (false: it's collapsed or off; End it all the same).
bool Begin(const char* name, bool* open = nullptr, int flags = 0);
void End();
bool IsWindowAppearing();
enum HoveredFlags : int { HoveredFlags_None = 0, HoveredFlags_AnyWindow = 1 };
bool IsWindowHovered(int flags = 0);
float GetWindowWidth();
void SetWindowFontScale(float scale);

// A scrolling area inside a window.
bool BeginChild(const char* id, Vec2 size = { 0, 0 }, bool border = false, int flags = 0);
void EndChild();

// --- Layout --------------------------------------------------------------------------

void SameLine(float offsetFromStart = 0.0f, float spacing = -1.0f);
void Spacing();
void Separator();
void SeparatorText(const char* text);
void Dummy(Vec2 size);
float GetCursorPosX();
void SetCursorPosX(float x);
Vec2 GetCursorScreenPos();
void AlignTextToFramePadding();
void SetNextItemWidth(float width); // (below 0: up to that far from the right edge)
void PushTextWrapPos(float x = 0.0f);
void PopTextWrapPos();
void PushID(int id);
void PushID(const char* id);
void PopID();
void BeginDisabled(bool disabled = true);
void EndDisabled();

enum StyleColor : int { Col_Text, Col_WindowBg, Col_Border, Col_Button, Col_COUNT };
void PushStyleColor(int which, Color c);
void PushStyleColor(int which, const Vec4& c);
void PopStyleColor(int count = 1);
enum StyleVar : int { StyleVar_WindowRounding, StyleVar_WindowPadding, StyleVar_ItemSpacing };
void PushStyleVar(int which, float value);
void PushStyleVar(int which, Vec2 value);
void PopStyleVar(int count = 1);
struct Style {
    Vec2 WindowPadding{ 10, 9 };
    Vec2 FramePadding{ 6, 4 };
    Vec2 ItemSpacing{ 8, 5 };
    float WindowRounding = 5.0f, FrameRounding = 3.0f;
    float ScrollbarSize = 12.0f;
};
Style& GetStyle();

// --- Widgets -------------------------------------------------------------------------

Vec2 CalcTextSize(const char* text, const char* end = nullptr, bool hideAfterDoubleHash = false, float wrapWidth = -1.0f);
void TextUnformatted(const char* text, const char* end = nullptr);
void Text(const char* fmt, ...);
void TextDisabled(const char* fmt, ...);
void TextColored(const Vec4& color, const char* fmt, ...);
void TextWrapped(const char* fmt, ...);
bool Button(const char* label, Vec2 size = { 0, 0 });
bool SmallButton(const char* label);
bool InvisibleButton(const char* id, Vec2 size);
bool Checkbox(const char* label, bool* value);
bool SliderFloat(const char* label, float* value, float min, float max, const char* format = "%.3f");
bool SliderInt(const char* label, int* value, int min, int max, const char* format = "%d");
bool InputInt(const char* label, int* value);
enum InputTextFlags : int { InputTextFlags_None = 0, InputTextFlags_EnterReturnsTrue = 1 };
// A line of text to type into (`buffer`, `size` bytes with its terminator). With
// EnterReturnsTrue it says true when Enter is pressed in it; else when it changes.
bool InputText(const char* label, char* buffer, size_t size, int flags = 0);
bool InputTextWithHint(const char* label, const char* hint, char* buffer, size_t size, int flags = 0);
bool Combo(const char* label, int* current, const char* const items[], int count);
void Image(TextureId texture, Vec2 size, Vec2 uv0 = { 0, 0 }, Vec2 uv1 = { 1, 1 });
enum TreeFlags : int { TreeFlags_None = 0, TreeFlags_DefaultOpen = 1 };
bool CollapsingHeader(const char* label, int flags = 0);
void SetNextItemOpen(bool open, int cond = Cond_Always);
void PlotLines(const char* label, const float* values, int count, int offset = 0, const char* overlay = nullptr, float min = 3.4e38f, float max = 3.4e38f,
               Vec2 size = { 0, 0 });
// Gives the next text box the keyboard.
void SetKeyboardFocusHere();

bool IsItemHovered();
bool IsAnyItemHovered();
bool IsAnyItemActive();
bool IsMouseClicked(int button);
bool IsMouseReleased(int button);
bool IsKeyPressed(Key key);
void SetTooltip(const char* fmt, ...);
void SetItemTooltip(const char* fmt, ...);

// Tables: columns side by side, rows down.
enum TableFlags : int { TableFlags_None = 0, TableFlags_RowBg = 1, TableFlags_BordersInnerH = 2, TableFlags_SizingFixedFit = 4, TableFlags_SizingStretchProp = 8 };
enum TableColumnFlags : int { TableColumnFlags_None = 0, TableColumnFlags_WidthFixed = 1, TableColumnFlags_WidthStretch = 2 };
bool BeginTable(const char* id, int columns, int flags = 0, Vec2 outerSize = { 0, 0 });
void TableSetupColumn(const char* label, int flags = 0, float width = 0.0f);
void TableNextRow();
bool TableNextColumn();
void EndTable();

// A modal popup: OpenPopup(id) once, then BeginPopupModal(id) each frame while it's up.
void OpenPopup(const char* id);
bool BeginPopupModal(const char* id, bool* open = nullptr, int flags = 0);
void EndPopup();
void CloseCurrentPopup();

} // namespace ui

#define UI_COL32(r, g, b, a) (::ui::Rgba((r), (g), (b), (a)))
#define UI_ARRAYSIZE(a) (static_cast<int>(sizeof(a) / sizeof(*(a))))
