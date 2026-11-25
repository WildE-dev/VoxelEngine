#include "ui/Ui.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stb_image.h>

#include "render/Shader.h"
#include "world/Json.h"

namespace ui {

Vec2 WhiteUV(); // (the white square in the font atlas, for solid shapes)

// --- Colours -----------------------------------------------------------------------

Color ToColor(const Vec4& c) {
    auto b = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return Rgba(b(c.x), b(c.y), b(c.z), b(c.w));
}
Vec4 ToVec4(Color c) { return { (c & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, ((c >> 24) & 255) / 255.0f }; }
namespace {
Color WithAlpha(Color c, float alpha) {
    const int a = static_cast<int>(std::lround(((c >> 24) & 255) * std::clamp(alpha, 0.0f, 1.0f)));
    return (c & 0x00FFFFFFu) | (static_cast<Color>(a) << 24);
}
// UTF-8: the next codepoint (bad bytes come out as one each).
unsigned NextChar(const char*& p, const char* end) {
    const unsigned char c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    unsigned cp = c & (0x3F >> extra);
    while (extra-- > 0 && p < end && (static_cast<unsigned char>(*p) & 0xC0) == 0x80) cp = (cp << 6) | (static_cast<unsigned char>(*p++) & 0x3F);
    return cp;
}
} // namespace

// --- Fonts -------------------------------------------------------------------------

Vec2 Font::Measure(float size, const char* text, const char* end, float wrapWidth) const {
    if (!end) end = text + std::strlen(text);
    const float scale = size / bakedSize, line = LineHeight(size);
    float x = 0.0f, widest = 0.0f, height = line;
    const char* p = text;
    while (p < end) {
        if (*p == '\n') {
            widest = std::max(widest, x), x = 0.0f, height += line, p++;
            continue;
        }
        if (wrapWidth > 0.0f && *p != ' ') {
            // The word starting here: onto the next line if it won't fit on this one.
            float word = 0.0f;
            for (const char* q = p; q < end && *q != ' ' && *q != '\n';) word += Get(NextChar(q, end)).advance * scale;
            if (x > 0.0f && x + word > wrapWidth) widest = std::max(widest, x), x = 0.0f, height += line;
        }
        x += Get(NextChar(p, end)).advance * scale;
    }
    return { std::max(widest, x), height };
}

// --- Draw lists --------------------------------------------------------------------

namespace {
struct Context;
Context* g = nullptr;
} // namespace

void DrawList::Clear() {
    vertices.clear(), indices.clear(), commands.clear(), clips.clear();
}
Vec4 DrawList::Clip() const { return clips.empty() ? Vec4(-1e6f, -1e6f, 1e6f, 1e6f) : clips.back(); }
void DrawList::PushClipRect(Vec2 a, Vec2 b, bool intersect) {
    Vec4 c(a.x, a.y, b.x, b.y);
    if (intersect && !clips.empty()) {
        const Vec4 p = clips.back();
        c = Vec4(std::max(c.x, p.x), std::max(c.y, p.y), std::min(c.z, p.z), std::min(c.w, p.w));
    }
    clips.push_back(c);
}
void DrawList::PopClipRect() {
    if (!clips.empty()) clips.pop_back();
}
void DrawList::Begin(TextureId texture) {
    const Vec4 clip = Clip();
    if (!commands.empty()) {
        const DrawCmd& last = commands.back();
        if (last.texture == texture && last.clip.x == clip.x && last.clip.y == clip.y && last.clip.z == clip.z && last.clip.w == clip.w) return;
    }
    commands.push_back({ texture, clip, static_cast<uint32_t>(indices.size()), 0 });
}
void DrawList::Quad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Vec2 uva, Vec2 uvb, Vec2 uvc, Vec2 uvd, Color ca, Color cb, Color cc, Color cd) {
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.push_back({ a, uva, ca });
    vertices.push_back({ b, uvb, cb });
    vertices.push_back({ c, uvc, cc });
    vertices.push_back({ d, uvd, cd });
    for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) indices.push_back(base + i);
    commands.back().indexCount += 6;
}
void DrawList::Fan(const std::vector<Vec2>& points, Color col) {
    if (points.size() < 3) return;
    Begin(0);
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    const Vec2 uv = WhiteUV();
    for (const Vec2& p : points) vertices.push_back({ p, uv, col });
    for (uint32_t i = 1; i + 1 < points.size(); i++) {
        indices.push_back(base), indices.push_back(base + i), indices.push_back(base + i + 1);
        commands.back().indexCount += 3;
    }
}
namespace {
// A rounded rectangle's outline points (clockwise from the top left).
std::vector<Vec2> RoundedRect(Vec2 a, Vec2 b, float r) {
    r = std::min({ r, (b.x - a.x) * 0.5f, (b.y - a.y) * 0.5f });
    std::vector<Vec2> pts;
    if (r <= 0.5f) return { a, { b.x, a.y }, b, { a.x, b.y } };
    const Vec2 centres[4] = { { a.x + r, a.y + r }, { b.x - r, a.y + r }, { b.x - r, b.y - r }, { a.x + r, b.y - r } };
    for (int c = 0; c < 4; c++)
        for (int i = 0; i <= 4; i++) {
            const float t = (180.0f + 90.0f * c + 22.5f * i) * 3.14159265f / 180.0f;
            pts.push_back({ centres[c].x + std::cos(t) * r, centres[c].y + std::sin(t) * r });
        }
    return pts;
}
} // namespace
void DrawList::AddRectFilled(Vec2 a, Vec2 b, Color c, float rounding, int) {
    if ((c >> 24) == 0 || b.x <= a.x || b.y <= a.y) return;
    if (rounding > 0.5f) return Fan(RoundedRect(a, b, rounding), c);
    Begin(0);
    const Vec2 uv = WhiteUV();
    Quad(a, { b.x, a.y }, b, { a.x, b.y }, uv, uv, uv, uv, c, c, c, c);
}
void DrawList::AddRectFilledMultiColor(Vec2 a, Vec2 b, Color tl, Color tr, Color br, Color bl) {
    Begin(0);
    const Vec2 uv = WhiteUV();
    Quad(a, { b.x, a.y }, b, { a.x, b.y }, uv, uv, uv, uv, tl, tr, br, bl);
}
void DrawList::AddPolyline(const Vec2* points, int count, Color col, float thickness) {
    for (int i = 0; i + 1 < count; i++) AddLine(points[i], points[i + 1], col, thickness);
}
void DrawList::AddRect(Vec2 a, Vec2 b, Color c, float rounding, int, float thickness) {
    if ((c >> 24) == 0) return;
    if (rounding > 0.5f) {
        std::vector<Vec2> pts = RoundedRect(a, b, rounding);
        pts.push_back(pts.front());
        return AddPolyline(pts.data(), static_cast<int>(pts.size()), c, thickness);
    }
    const float h = thickness * 0.5f;
    AddRectFilled({ a.x - h, a.y - h }, { b.x + h, a.y + h }, c);
    AddRectFilled({ a.x - h, b.y - h }, { b.x + h, b.y + h }, c);
    AddRectFilled({ a.x - h, a.y + h }, { a.x + h, b.y - h }, c);
    AddRectFilled({ b.x - h, a.y + h }, { b.x + h, b.y - h }, c);
}
void DrawList::AddLine(Vec2 a, Vec2 b, Color c, float thickness) {
    if ((c >> 24) == 0) return;
    const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float nx = -dy / len * thickness * 0.5f, ny = dx / len * thickness * 0.5f;
    Begin(0);
    const Vec2 uv = WhiteUV();
    Quad({ a.x + nx, a.y + ny }, { b.x + nx, b.y + ny }, { b.x - nx, b.y - ny }, { a.x - nx, a.y - ny }, uv, uv, uv, uv, c, c, c, c);
}
void DrawList::AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, Color col) { Fan({ a, b, c }, col); }
void DrawList::AddQuadFilled(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color col) { Fan({ a, b, c, d }, col); }
void DrawList::AddCircleFilled(Vec2 centre, float radius, Color col, int segments) {
    if (segments <= 0) segments = std::clamp(static_cast<int>(radius * 1.2f), 12, 48);
    std::vector<Vec2> pts;
    for (int i = 0; i < segments; i++) {
        const float t = 6.2831853f * i / segments;
        pts.push_back({ centre.x + std::cos(t) * radius, centre.y + std::sin(t) * radius });
    }
    Fan(pts, col);
}
void DrawList::AddCircle(Vec2 centre, float radius, Color col, int segments, float thickness) {
    if (segments <= 0) segments = std::clamp(static_cast<int>(radius * 1.2f), 12, 48);
    std::vector<Vec2> pts;
    for (int i = 0; i <= segments; i++) {
        const float t = 6.2831853f * i / segments;
        pts.push_back({ centre.x + std::cos(t) * radius, centre.y + std::sin(t) * radius });
    }
    AddPolyline(pts.data(), static_cast<int>(pts.size()), col, thickness);
}
void DrawList::AddImage(TextureId texture, Vec2 a, Vec2 b, Vec2 uv0, Vec2 uv1, Color col) {
    Begin(texture);
    Quad(a, { b.x, a.y }, b, { a.x, b.y }, uv0, { uv1.x, uv0.y }, uv1, { uv0.x, uv1.y }, col, col, col, col);
}
void DrawList::AddText(Vec2 pos, Color col, const char* text, const char* end) { AddText(GetFont(), GetFontSize(), pos, col, text, end, 0.0f); }
void DrawList::AddText(const Font* font, float size, Vec2 pos, Color col, const char* text, const char* end, float wrapWidth) {
    if (!font || (col >> 24) == 0 || !text) return;
    if (!end) end = text + std::strlen(text);
    Begin(0);
    const float scale = size / font->bakedSize, line = font->LineHeight(size), inv = 1.0f / font->atlasSize;
    float x = pos.x, y = std::round(pos.y);
    const char* p = text;
    while (p < end) {
        if (*p == '\n') {
            x = pos.x, y += line, p++;
            continue;
        }
        if (wrapWidth > 0.0f && *p != ' ') {
            float word = 0.0f;
            for (const char* q = p; q < end && *q != ' ' && *q != '\n';) word += font->Get(NextChar(q, end)).advance * scale;
            if (x > pos.x && x - pos.x + word > wrapWidth) x = pos.x, y += line;
        }
        const Glyph& gl = font->Get(NextChar(p, end));
        if (gl.w > 0.0f) {
            const float x0 = std::round(x + gl.xoff * scale), y0 = y + gl.yoff * scale;
            const Vec2 a(x0, y0), b(x0 + gl.w * scale, y0 + gl.h * scale);
            const Vec2 uv0(gl.x * inv, gl.y * inv), uv1((gl.x + gl.w) * inv, (gl.y + gl.h) * inv);
            Quad(a, { b.x, a.y }, b, { a.x, b.y }, uv0, { uv1.x, uv0.y }, uv1, { uv0.x, uv1.y }, col, col, col, col);
        }
        x += gl.advance * scale;
    }
}

// --- The context -------------------------------------------------------------------

namespace {

struct Rect {
    Vec2 min, max;
    bool Contains(Vec2 p) const { return p.x >= min.x && p.y >= min.y && p.x < max.x && p.y < max.y; }
};

// Where items go: a window's inside, or a scrolling area, or a table cell.
struct Region {
    uint32_t id = 0;
    Vec2 origin;          // where the content starts (screen), before scrolling
    float lineStartX = 0; // where each line starts (an indent, a table column)
    float width = 0;      // how wide items may go
    float bottom = 1e9f;  // where the area ends (for "the rest of the height")
    Vec2 cursor;
    float lineTop = 0, lineHeight = 0;
    float prevLineTop = 0, prevLineHeight = 0;
    Rect lastItem;
    Vec2 contentMax;
    bool alignText = false;
    // A scrolling area: its box, how far it's scrolled, and its content's height.
    bool child = false, border = false;
    Rect box;
    float scroll = 0;
};

struct Window {
    std::string name;
    uint32_t id = 0;
    int flags = 0;
    Vec2 pos, size;
    Vec2 contentSize; // (last frame's, for sizing to fit)
    bool seenLast = false, seenNow = false, appearing = true, hidden = false;
    int layer = 0; // 0 windows, 1 popups
    int order = 0;
    float fontScale = 1.0f;
    DrawList draw;
    Rect rect() const { return { pos, pos + size }; }
};

struct TableState {
    std::vector<float> measured, fixed, rowHeights;
    std::vector<int> columnFlags;
};
struct Table {
    uint32_t id = 0;
    int columns = 0, flags = 0, column = -1, row = -1;
    float x = 0, width = 0, top = 0, rowTop = 0, rowBottom = 0;
    std::vector<float> colX, colW;
    TableState* state = nullptr;
    int setupIndex = 0;
    Region saved;
};

struct TextEdit {
    uint32_t id = 0;
    std::string text;
    size_t cursor = 0;
    float blink = 0;
};

struct NavItem {
    uint32_t id;
    Rect rect;
};

struct Context {
    IO io;
    Style style;
    Font font;
    Viewport viewport;
    float deltaTime = 0.016f, time = 0;
    // Input this frame.
    bool mouseClicked[3] = {}, mouseReleased[3] = {}, mouseWasDown[3] = {};
    std::vector<Key> keys;
    std::vector<unsigned> chars;
    bool ctrlV = false;
    const char* (*clipboard)(void*) = nullptr;
    void* clipboardUser = nullptr;
    bool padNow[6] = {}, padWas[6] = {}, padPressed[6] = {};
    // Windows.
    std::map<uint32_t, std::unique_ptr<Window>> windows;
    std::vector<Window*> order, lastOrder;
    Window* current = nullptr;
    std::vector<Window*> windowStack;
    uint32_t hoveredWindow = 0;
    std::vector<Region> regions;
    std::vector<uint32_t> idStack;
    // Next window's settings.
    bool nextPos = false, nextSize = false, nextConstraint = false;
    int nextPosCond = 0, nextSizeCond = 0;
    Vec2 nextPosValue, nextPivot, nextSizeValue, nextMin, nextMax;
    float nextBgAlpha = -1.0f;
    float nextItemWidth = 0.0f;
    bool nextItemOpenSet = false, nextItemOpen = false;
    int nextItemOpenCond = 0;
    bool focusNextText = false;
    // Items.
    uint32_t activeId = 0, hotId = 0;
    uint32_t lastItemId = 0;
    bool lastItemHovered = false, anyHovered = false, anyActive = false, clickedAnItem = false;
    int disabled = 0;
    std::vector<bool> disabledStack;
    uint32_t wheelTarget = 0, wheelCandidate = 0;
    std::vector<float> wrapStack;
    std::vector<std::pair<int, Color>> colorStack;
    Color colors[Col_COUNT];
    std::vector<std::pair<int, Vec2>> varStack;
    // Text boxes.
    TextEdit edit;
    // Drop-down lists, collapsing sections, popups, tables.
    uint32_t comboOpen = 0;
    Rect comboRect, comboRectLast;
    std::unordered_map<uint32_t, bool> opened;
    std::unordered_map<uint32_t, bool> popupsOpen;
    std::vector<uint32_t> popupStack;
    bool modalLast = false, modalNow = false;
    std::unordered_map<uint32_t, TableState> tables;
    std::vector<Table> tableStack;
    std::unordered_map<uint32_t, float> scrolls;
    std::unordered_map<uint32_t, float> contentHeights;
    // Tooltips and the layers over everything.
    std::string tooltip;
    DrawList background, foreground, overlay;
    // The gamepad's focus.
    std::vector<NavItem> navItems, navLast;
    uint32_t navFocus = 0;
    bool navActivate = false, navVisible = false;
    int navDelta = 0;
    // Rendering.
    std::unique_ptr<Shader> shader;
    GLuint fontTexture = 0, vao = 0, vbo = 0, ebo = 0;
};

uint32_t Hash(const char* s, size_t n, uint32_t seed) {
    uint32_t h = seed ^ 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<unsigned char>(s[i])) * 16777619u;
    return h ? h : 1;
}
uint32_t Seed() { return g->idStack.empty() ? 0x9E3779B9u : g->idStack.back(); }
// An id from a label: all of it, or what follows "###", ignoring what follows "##".
uint32_t IdOf(const char* label) {
    const char* triple = std::strstr(label, "###");
    if (triple) return Hash(triple, std::strlen(triple), Seed());
    return Hash(label, std::strlen(label), Seed());
}
// What of a label is shown (up to any "##").
std::string Shown(const char* label) {
    const char* hash = std::strstr(label, "##");
    return hash ? std::string(label, hash) : std::string(label);
}
Region& R() { return g->regions.back(); }
float FontSize() { return g->io.FontSize * (g->current ? g->current->fontScale : 1.0f); }
float LineHeight() { return g->font.LineHeight(FontSize()); }
float FrameHeight() { return LineHeight() + g->style.FramePadding.y * 2.0f; }
DrawList& DL() { return g->current ? g->current->draw : g->foreground; }
float Alpha() { return g->disabled > 0 ? 0.45f : 1.0f; }
Color C(Color c) { return WithAlpha(c, Alpha()); }
Color TextColor() { return C(g->colors[Col_Text]); }
Color DimText() { return C(Rgba(150, 150, 158)); }

// Theme: dark glassy panels, a warm accent.
constexpr Color kFrame = Rgba(46, 48, 58, 230), kFrameHover = Rgba(66, 70, 86, 240), kFrameActive = Rgba(86, 92, 112, 250);
constexpr Color kAccent = Rgba(228, 178, 84), kAccentDim = Rgba(150, 118, 60);
constexpr Color kTitle = Rgba(30, 32, 40, 245);

void ResetColors() {
    g->colors[Col_Text] = Rgba(236, 236, 240);
    g->colors[Col_WindowBg] = Rgba(18, 19, 24, 235);
    g->colors[Col_Border] = Rgba(255, 255, 255, 40);
    g->colors[Col_Button] = Rgba(58, 62, 78, 235);
}

// Places an item of `size` at the cursor; its box.
Rect ItemAdd(Vec2 size) {
    Region& r = R();
    Rect rc{ { r.cursor.x, r.lineTop }, { r.cursor.x + size.x, r.lineTop + size.y } };
    r.lastItem = rc;
    r.prevLineTop = r.lineTop;
    r.prevLineHeight = std::max(r.lineHeight, size.y);
    r.contentMax = { std::max(r.contentMax.x, rc.max.x), std::max(r.contentMax.y, rc.max.y) };
    r.lineTop = r.prevLineTop + r.prevLineHeight + g->style.ItemSpacing.y;
    r.lineHeight = 0;
    r.cursor.x = r.lineStartX;
    r.alignText = false;
    return rc;
}
float AvailableWidth() {
    const Region& r = R();
    return std::max(1.0f, r.lineStartX + r.width - r.cursor.x);
}
float ItemWidth() {
    float w = g->nextItemWidth;
    g->nextItemWidth = 0.0f;
    if (w < 0.0f) return std::max(10.0f, AvailableWidth() + w);
    if (w > 0.0f) return w;
    return std::max(160.0f, R().width * 0.62f);
}
bool InClip(Vec2 p) {
    const Region& r = R();
    if (r.child && !r.box.Contains(p)) return false;
    for (const Region& outer : g->regions)
        if (outer.child && !outer.box.Contains(p)) return false;
    return true;
}
bool Hoverable(const Rect& rc) {
    if (g->disabled > 0 || (g->io.ConfigFlags & ConfigFlags_NoMouse)) return false;
    if (!g->current || g->hoveredWindow != g->current->id) return false;
    if (g->activeId != 0 && g->activeId != g->lastItemId) return false;
    if (g->comboOpen && g->comboRectLast.Contains(g->io.MousePos)) return false;
    return rc.Contains(g->io.MousePos) && InClip(g->io.MousePos);
}
void NavAdd(uint32_t id, const Rect& rc) {
    if (g->disabled > 0 || !g->current || (g->current->flags & WindowFlags_NoNav)) return;
    if (g->modalNow && g->current->layer != 1) return;
    g->navItems.push_back({ id, rc });
    if (g->navVisible && g->navFocus == id) DL().AddRect(rc.min - Vec2(2, 2), rc.max + Vec2(2, 2), kAccent, 3.0f, 0, 2.0f);
}
// A button's behaviour: hovered, held, and pressed (let go over it, or A on it).
bool Behaviour(uint32_t id, const Rect& rc, bool& hovered, bool& held) {
    g->lastItemId = id;
    hovered = Hoverable(rc);
    g->lastItemHovered = hovered;
    if (hovered) g->anyHovered = true;
    bool pressed = false;
    if (hovered && g->mouseClicked[0]) g->activeId = id, g->clickedAnItem = true;
    held = g->activeId == id && g->io.MouseDown[0];
    if (g->activeId == id && g->mouseReleased[0]) {
        pressed = hovered;
        g->activeId = 0;
    }
    if (g->activeId == id) g->anyActive = true;
    NavAdd(id, rc);
    if (g->navActivate && g->navFocus == id && g->disabled == 0) pressed = true, g->navActivate = false;
    return pressed;
}
void DrawFrame(const Rect& rc, bool hovered, bool held, Color base = kFrame) {
    DL().AddRectFilled(rc.min, rc.max, C(held ? kFrameActive : hovered ? kFrameHover : base), g->style.FrameRounding);
}
void Label(const Rect& frame, const std::string& label) {
    if (label.empty()) return;
    DL().AddText(GetFont(), FontSize(), { frame.max.x + g->style.ItemSpacing.x, frame.min.y + g->style.FramePadding.y }, TextColor(), label.c_str());
    R().contentMax.x = std::max(R().contentMax.x, frame.max.x + g->style.ItemSpacing.x + CalcTextSize(label.c_str()).x);
}
std::string Format(const char* fmt, va_list args) {
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, fmt, args);
    return buf;
}
void TextItem(const char* text, const char* end, Color col, bool wrap) {
    float wrapX = 0.0f;
    if (wrap) wrapX = AvailableWidth();
    else if (!g->wrapStack.empty()) {
        const float w = g->wrapStack.back();
        wrapX = w > 0.0f ? std::max(10.0f, w - R().cursor.x) : AvailableWidth();
    }
    const bool align = R().alignText;
    const Vec2 size = g->font.Measure(FontSize(), text, end, wrapX);
    const float pad = align ? g->style.FramePadding.y : 0.0f;
    const Rect rc = ItemAdd({ size.x, size.y + pad * 2.0f });
    DL().AddText(GetFont(), FontSize(), { rc.min.x, rc.min.y + pad }, col, text, end, wrapX);
    g->lastItemId = 0;
    g->lastItemHovered = Hoverable(rc);
}

void StartRegion(Region& r, Vec2 origin, float width, float bottom) {
    r.origin = origin;
    r.cursor = origin;
    r.lineStartX = origin.x;
    r.width = width;
    r.bottom = bottom;
    r.lineTop = r.prevLineTop = origin.y;
    r.contentMax = origin;
}

} // namespace

Vec2 WhiteUV() { return g ? Vec2(g->font.white.x / g->font.atlasSize, g->font.white.y / g->font.atlasSize) : Vec2(); }

// --- Setup and input ---------------------------------------------------------------

bool Init(const std::string& dir, std::string& error) {
    delete g;
    g = new Context();
    ResetColors();
    // The font's metrics, then its atlas.
    std::ifstream file(dir + "/ui.json");
    if (!file) return error = "can't read " + dir + "/ui.json", false;
    std::stringstream ss;
    ss << file.rdbuf();
    Json::Value v;
    if (!Json::Parser(ss.str()).Parse(v, error)) return false;
    Font& f = g->font;
    f.bakedSize = static_cast<float>(v.NumberOr("size", 32));
    f.ascent = static_cast<float>(v.NumberOr("ascent", 32));
    f.descent = static_cast<float>(v.NumberOr("descent", 8));
    f.atlasSize = static_cast<float>(v.NumberOr("atlas", 512));
    const int missing = static_cast<int>(v.NumberOr("missing", 0x25A1));
    if (const Json::Value* glyphs = v.Find("glyphs"))
        for (const auto& [key, gv] : glyphs->object) {
            if (!gv.IsArray() || gv.array.size() < 7) continue;
            Glyph gl{ static_cast<float>(gv.array[0].number), static_cast<float>(gv.array[1].number), static_cast<float>(gv.array[2].number),
                      static_cast<float>(gv.array[3].number), static_cast<float>(gv.array[4].number), static_cast<float>(gv.array[5].number),
                      static_cast<float>(gv.array[6].number) };
            const int code = std::atoi(key.c_str());
            if (code >= 0 && code < 128) f.glyphs[code] = gl;
            if (code == missing) f.missing = gl;
        }
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load((dir + "/ui.png").c_str(), &w, &h, &channels, 4);
    if (!pixels) return error = "can't read " + dir + "/ui.png", false;
    glGenTextures(1, &g->fontTexture);
    glBindTexture(GL_TEXTURE_2D, g->fontTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(pixels);
    g->shader = std::make_unique<Shader>("Resources/Shaders/ui.vert", "Resources/Shaders/ui.frag");
    glGenVertexArrays(1, &g->vao);
    glGenBuffers(1, &g->vbo);
    glGenBuffers(1, &g->ebo);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, uv)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, color)));
    glBindVertexArray(0);
    return true;
}

void Shutdown() {
    if (!g) return;
    if (g->fontTexture) glDeleteTextures(1, &g->fontTexture);
    if (g->vao) glDeleteVertexArrays(1, &g->vao);
    if (g->vbo) glDeleteBuffers(1, &g->vbo);
    if (g->ebo) glDeleteBuffers(1, &g->ebo);
    delete g;
    g = nullptr;
}

IO& GetIO() { return g->io; }
void AddKey(Key key, bool ctrl) {
    if (ctrl) return;
    g->keys.push_back(key);
}
void AddChar(unsigned codepoint) {
    if (codepoint == 22) g->ctrlV = true; // (Ctrl+V, from the game)
    else if (codepoint >= 32) g->chars.push_back(codepoint);
}
void AddMouseButton(int button, bool down) {
    if (button < 0 || button > 2) return;
    if (down && !g->io.MouseDown[button]) g->mouseClicked[button] = true;
    if (!down && g->io.MouseDown[button]) g->mouseReleased[button] = true;
    g->io.MouseDown[button] = down;
}
void AddMouseWheel(float delta) { g->io.MouseWheel += delta; }
void SetMousePos(Vec2 pos) { g->io.MousePos = pos; }
void SetGamepad(bool up, bool down, bool left, bool right, bool a, bool b) {
    const bool now[6] = { up, down, left, right, a, b };
    for (int i = 0; i < 6; i++) g->padNow[i] = now[i];
}
void SetClipboardGetter(const char* (*get)(void*), void* user) { g->clipboard = get, g->clipboardUser = user; }

const Viewport* GetMainViewport() { return &g->viewport; }
DrawList* GetForegroundDrawList() { return &g->foreground; }
DrawList* GetBackgroundDrawList() { return &g->background; }
DrawList* GetWindowDrawList() { return &DL(); }
const Font* GetFont() { return &g->font; }
float GetFontSize() { return g ? FontSize() : 15.0f; }
Style& GetStyle() { return g->style; }

void NewFrame(Vec2 displaySize, float deltaTime) {
    g->io.DisplaySize = displaySize;
    g->viewport = { { 0, 0 }, displaySize, { 0, 0 }, displaySize };
    g->deltaTime = deltaTime;
    g->time += deltaTime;
    // The window under the mouse: the topmost of last frame's (a modal popup's, if one's up).
    g->hoveredWindow = 0;
    if (!(g->io.ConfigFlags & ConfigFlags_NoMouse))
        for (auto it = g->lastOrder.rbegin(); it != g->lastOrder.rend(); ++it) {
            Window* w = *it;
            if (g->modalLast && w->layer != 1) continue;
            if (!(w->flags & WindowFlags_NoInputs) && !w->hidden && w->rect().Contains(g->io.MousePos)) {
                g->hoveredWindow = w->id;
                break;
            }
        }
    g->io.WantCaptureMouse = g->hoveredWindow != 0 || g->activeId != 0;
    // The gamepad: what's newly pressed, and moving the focus along last frame's items.
    for (int i = 0; i < 6; i++) g->padPressed[i] = g->padNow[i] && !g->padWas[i], g->padWas[i] = g->padNow[i];
    g->navActivate = false, g->navDelta = 0;
    if (g->padPressed[0] || g->padPressed[1] || g->padPressed[2] || g->padPressed[3] || g->padPressed[4]) g->navVisible = true;
    if (g->mouseClicked[0]) g->navVisible = false;
    if (!g->navLast.empty()) {
        int at = -1;
        for (size_t i = 0; i < g->navLast.size(); i++)
            if (g->navLast[i].id == g->navFocus) at = static_cast<int>(i);
        if (at < 0 && (g->padPressed[0] || g->padPressed[1] || g->padPressed[4])) at = 0, g->navFocus = g->navLast[0].id;
        else if (at >= 0) {
            const int n = static_cast<int>(g->navLast.size());
            if (g->padPressed[1]) g->navFocus = g->navLast[(at + 1) % n].id;
            if (g->padPressed[0]) g->navFocus = g->navLast[(at + n - 1) % n].id;
            if (g->padPressed[4]) g->navActivate = true;
            if (g->padPressed[2]) g->navDelta = -1;
            if (g->padPressed[3]) g->navDelta = 1;
        }
    }
    g->navItems.clear();
    g->wheelTarget = g->wheelCandidate, g->wheelCandidate = 0;
    // Fresh lists.
    g->background.Clear(), g->foreground.Clear(), g->overlay.Clear();
    g->order.clear();
    g->tooltip.clear();
    g->anyHovered = g->anyActive = g->clickedAnItem = false;
    g->modalNow = false;
    g->comboRectLast = g->comboRect;
    g->comboRect = {};
    for (auto& [id, w] : g->windows) w->seenNow = false;
    g->current = nullptr;
}

// --- Windows -----------------------------------------------------------------------

void SetNextWindowPos(Vec2 pos, int cond, Vec2 pivot) { g->nextPos = true, g->nextPosValue = pos, g->nextPivot = pivot, g->nextPosCond = cond; }
void SetNextWindowSize(Vec2 size, int cond) { g->nextSize = true, g->nextSizeValue = size, g->nextSizeCond = cond; }
void SetNextWindowSizeConstraints(Vec2 min, Vec2 max) { g->nextConstraint = true, g->nextMin = min, g->nextMax = max; }
void SetNextWindowBgAlpha(float alpha) { g->nextBgAlpha = alpha; }

namespace {
bool BeginWindow(const char* name, bool* open, int flags, int layer) {
    const uint32_t id = Hash(name, std::strlen(name), 0x51ED270Bu);
    auto& slot = g->windows[id];
    if (!slot) slot = std::make_unique<Window>(), slot->name = name, slot->id = id;
    Window* w = slot.get();
    w->flags = flags;
    w->layer = layer;
    w->appearing = !w->seenLast;
    w->seenNow = true;
    w->fontScale = 1.0f;
    w->draw.Clear();
    w->order = static_cast<int>(g->order.size());
    g->order.push_back(w);
    const Style& s = g->style;
    const bool title = !(flags & WindowFlags_NoTitleBar);
    const float titleH = title ? g->font.LineHeight(g->io.FontSize) + 8.0f : 0.0f;
    // Its size: as asked, or to fit what was in it last time.
    // (A size of 0 either way: that way fits what's in it.)
    const Vec2 fit{ w->contentSize.x + s.WindowPadding.x * 2.0f, w->contentSize.y + s.WindowPadding.y * 2.0f + titleH };
    bool fitted = false;
    if (g->nextSize && (g->nextSizeCond == Cond_Always || w->appearing)) {
        w->size = { g->nextSizeValue.x > 0.0f ? g->nextSizeValue.x : fit.x, g->nextSizeValue.y > 0.0f ? g->nextSizeValue.y : fit.y };
        fitted = g->nextSizeValue.x <= 0.0f || g->nextSizeValue.y <= 0.0f;
    }
    else if ((flags & WindowFlags_AlwaysAutoResize) || w->size.x <= 0.0f) w->size = fit, fitted = true;
    if (g->nextConstraint) w->size = { std::clamp(w->size.x, g->nextMin.x, g->nextMax.x), std::clamp(w->size.y, g->nextMin.y, g->nextMax.y) };
    if (title) w->size.x = std::max(w->size.x, CalcTextSize(Shown(name).c_str()).x + 60.0f);
    if (g->nextPos && (g->nextPosCond == Cond_Always || w->appearing))
        w->pos = { std::round(g->nextPosValue.x - w->size.x * g->nextPivot.x), std::round(g->nextPosValue.y - w->size.y * g->nextPivot.y) };
    // (Sized to fit, it shows from its second frame, once it knows its size.)
    w->hidden = fitted && w->appearing;
    const float bgAlpha = g->nextBgAlpha;
    g->nextPos = g->nextSize = g->nextConstraint = false;
    g->nextBgAlpha = -1.0f;
    g->windowStack.push_back(w);
    g->current = w;
    g->idStack.push_back(id);
    DrawList& d = w->draw;
    const Rect rc = w->rect();
    if (!(flags & WindowFlags_NoBackground)) {
        Color bg = g->colors[Col_WindowBg];
        if (bgAlpha >= 0.0f) bg = WithAlpha(bg | 0xFF000000u, bgAlpha);
        d.AddRectFilled(rc.min, rc.max, bg, s.WindowRounding);
        d.AddRect(rc.min, rc.max, g->colors[Col_Border], s.WindowRounding);
    }
    bool keep = true;
    if (title) {
        d.AddRectFilled(rc.min, { rc.max.x, rc.min.y + titleH }, kTitle, s.WindowRounding);
        d.AddRectFilled({ rc.min.x, rc.min.y + titleH - 2.0f }, { rc.max.x, rc.min.y + titleH }, kAccentDim);
        d.AddText(GetFont(), g->io.FontSize, { rc.min.x + s.WindowPadding.x, rc.min.y + 4.0f }, Rgba(250, 240, 220), Shown(name).c_str());
        if (open) {
            // The close button: an X at the right of the title.
            const float bs = titleH - 8.0f;
            const Rect close{ { rc.max.x - bs - 6.0f, rc.min.y + 4.0f }, { rc.max.x - 6.0f, rc.min.y + 4.0f + bs } };
            const bool hover = g->hoveredWindow == id && close.Contains(g->io.MousePos);
            if (hover) d.AddRectFilled(close.min, close.max, Rgba(200, 70, 60, 200), 3.0f);
            const Vec2 c((close.min.x + close.max.x) * 0.5f, (close.min.y + close.max.y) * 0.5f);
            const float r = bs * 0.25f;
            d.AddLine({ c.x - r, c.y - r }, { c.x + r, c.y + r }, Rgba(240, 240, 240), 2.0f);
            d.AddLine({ c.x - r, c.y + r }, { c.x + r, c.y - r }, Rgba(240, 240, 240), 2.0f);
            if (hover && g->mouseClicked[0]) keep = false, g->clickedAnItem = true;
        }
    }
    if (open && !keep) *open = false;
    // Its inside: items from the top left, clipped to it.
    Region r;
    r.id = id;
    const Vec2 origin(rc.min.x + s.WindowPadding.x, rc.min.y + titleH + s.WindowPadding.y);
    const float innerW = (flags & WindowFlags_AlwaysAutoResize) && w->contentSize.x > 0.0f ? std::max(w->contentSize.x, w->size.x - s.WindowPadding.x * 2.0f)
                                                                                       : w->size.x - s.WindowPadding.x * 2.0f;
    StartRegion(r, origin, innerW, rc.max.y - s.WindowPadding.y);
    g->regions.push_back(r);
    d.PushClipRect({ rc.min.x + 1.0f, rc.min.y + titleH }, { rc.max.x - 1.0f, rc.max.y - 1.0f }, false);
    return true;
}
void EndWindow() {
    Window* w = g->current;
    if (!w) return;
    const Region& r = R();
    w->contentSize = { std::max(0.0f, r.contentMax.x - r.origin.x), std::max(0.0f, r.contentMax.y - r.origin.y) };
    w->draw.PopClipRect();
    g->regions.pop_back();
    g->idStack.pop_back();
    g->windowStack.pop_back();
    g->current = g->windowStack.empty() ? nullptr : g->windowStack.back();
}
} // namespace

bool Begin(const char* name, bool* open, int flags) { return BeginWindow(name, open, flags, 0); }
void End() { EndWindow(); }
bool IsWindowAppearing() { return g->current && g->current->appearing; }
bool IsWindowHovered(int flags) {
    if (flags & HoveredFlags_AnyWindow) return g->hoveredWindow != 0;
    return g->current && g->hoveredWindow == g->current->id;
}
float GetWindowWidth() { return g->current ? g->current->size.x : g->viewport.Size.x; }
void SetWindowFontScale(float scale) {
    if (g->current) g->current->fontScale = scale;
}

bool BeginChild(const char* idText, Vec2 size, bool border, int) {
    const uint32_t id = IdOf(idText);
    Region& parent = R();
    const float w = size.x > 0.0f ? size.x : AvailableWidth() + size.x;
    const float h = size.y > 0.0f ? size.y : std::max(20.0f, parent.bottom - parent.lineTop + size.y);
    const Rect box = ItemAdd({ w, h });
    if (border) DL().AddRect(box.min, box.max, Rgba(255, 255, 255, 30), 3.0f);
    Region r;
    r.id = id;
    r.child = true;
    r.border = border;
    r.box = box;
    const float pad = border ? 6.0f : 0.0f;
    float& scroll = g->scrolls[id];
    const float contentH = g->contentHeights[id];
    const float maxScroll = std::max(0.0f, contentH - (h - pad * 2.0f));
    // The wheel over it scrolls it (the innermost one under the mouse).
    if (box.Contains(g->io.MousePos) && InClip(g->io.MousePos) && g->current && g->hoveredWindow == g->current->id) g->wheelCandidate = id;
    if (id == g->wheelTarget && g->io.MouseWheel != 0.0f) scroll -= g->io.MouseWheel * LineHeight() * 3.0f;
    scroll = std::clamp(scroll, 0.0f, maxScroll);
    r.scroll = scroll;
    const float bar = maxScroll > 0.0f ? g->style.ScrollbarSize : 0.0f;
    StartRegion(r, { box.min.x + pad, box.min.y + pad - scroll }, w - pad * 2.0f - bar, box.max.y - pad);
    if (maxScroll > 0.0f) {
        // The scrollbar: a thumb showing how much is in view.
        const float trackH = h - 4.0f, thumbH = std::max(18.0f, trackH * (h / (contentH + pad * 2.0f)));
        const float y = box.min.y + 2.0f + (trackH - thumbH) * (scroll / maxScroll);
        DL().AddRectFilled({ box.max.x - bar + 2.0f, box.min.y + 2.0f }, { box.max.x - 2.0f, box.max.y - 2.0f }, Rgba(0, 0, 0, 60), 4.0f);
        DL().AddRectFilled({ box.max.x - bar + 3.0f, y }, { box.max.x - 3.0f, y + thumbH }, Rgba(200, 200, 210, 120), 4.0f);
    }
    g->regions.push_back(r);
    g->idStack.push_back(id);
    DL().PushClipRect(box.min, box.max);
    return true;
}
void EndChild() {
    const Region r = R();
    g->contentHeights[r.id] = std::max(0.0f, r.contentMax.y - r.origin.y);
    DL().PopClipRect();
    g->idStack.pop_back();
    g->regions.pop_back();
}

// --- Layout ------------------------------------------------------------------------

void SameLine(float offset, float spacing) {
    Region& r = R();
    r.lineTop = r.prevLineTop;
    r.lineHeight = r.prevLineHeight;
    r.cursor.x = offset > 0.0f ? r.origin.x + offset : r.lastItem.max.x + (spacing < 0.0f ? g->style.ItemSpacing.x : spacing);
}
void Spacing() { ItemAdd({ 0.0f, g->style.ItemSpacing.y }); }
void Dummy(Vec2 size) { ItemAdd(size); }
void Separator() {
    const Rect rc = ItemAdd({ R().width, 3.0f });
    DL().AddLine({ rc.min.x, rc.min.y + 1.5f }, { rc.min.x + R().width, rc.min.y + 1.5f }, Rgba(255, 255, 255, 40));
}
void SeparatorText(const char* text) {
    const Vec2 size = CalcTextSize(text);
    const Rect rc = ItemAdd({ R().width, size.y + 4.0f });
    const float y = rc.min.y + size.y * 0.55f + 2.0f;
    DL().AddLine({ rc.min.x, y }, { rc.min.x + 10.0f, y }, Rgba(255, 255, 255, 50));
    DL().AddText(GetFont(), FontSize(), { rc.min.x + 16.0f, rc.min.y + 2.0f }, C(kAccent), text);
    DL().AddLine({ rc.min.x + 22.0f + size.x, y }, { rc.min.x + R().width, y }, Rgba(255, 255, 255, 50));
}
float GetCursorPosX() { return R().cursor.x - (g->current ? g->current->pos.x : 0.0f); }
void SetCursorPosX(float x) { R().cursor.x = (g->current ? g->current->pos.x : 0.0f) + x; }
Vec2 GetCursorScreenPos() { return { R().cursor.x, R().lineTop }; }
void AlignTextToFramePadding() { R().alignText = true; }
void SetNextItemWidth(float width) { g->nextItemWidth = width; }
void PushTextWrapPos(float x) { g->wrapStack.push_back(x > 0.0f && g->current ? g->current->pos.x + x : 0.0f); } // (0: the right edge)
void PopTextWrapPos() {
    if (!g->wrapStack.empty()) g->wrapStack.pop_back();
}
void PushID(int id) { g->idStack.push_back(Hash(reinterpret_cast<const char*>(&id), sizeof id, Seed())); }
void PushID(const char* id) { g->idStack.push_back(Hash(id, std::strlen(id), Seed())); }
void PopID() {
    if (!g->idStack.empty()) g->idStack.pop_back();
}
void BeginDisabled(bool disabled) {
    g->disabledStack.push_back(disabled);
    if (disabled) g->disabled++;
}
void EndDisabled() {
    if (g->disabledStack.empty()) return;
    if (g->disabledStack.back()) g->disabled--;
    g->disabledStack.pop_back();
}
void PushStyleColor(int which, Color c) {
    if (which < 0 || which >= Col_COUNT) return;
    g->colorStack.push_back({ which, g->colors[which] });
    g->colors[which] = c;
}
void PushStyleColor(int which, const Vec4& c) { PushStyleColor(which, ToColor(c)); }
void PopStyleColor(int count) {
    while (count-- > 0 && !g->colorStack.empty()) {
        g->colors[g->colorStack.back().first] = g->colorStack.back().second;
        g->colorStack.pop_back();
    }
}
void PushStyleVar(int which, float value) {
    if (which == StyleVar_WindowRounding) g->varStack.push_back({ which, { g->style.WindowRounding, 0 } }), g->style.WindowRounding = value;
}
void PushStyleVar(int which, Vec2 value) {
    Vec2& v = which == StyleVar_WindowPadding ? g->style.WindowPadding : g->style.ItemSpacing;
    if (which != StyleVar_WindowPadding && which != StyleVar_ItemSpacing) return;
    g->varStack.push_back({ which, v });
    v = value;
}
void PopStyleVar(int count) {
    while (count-- > 0 && !g->varStack.empty()) {
        auto [which, v] = g->varStack.back();
        g->varStack.pop_back();
        if (which == StyleVar_WindowRounding) g->style.WindowRounding = v.x;
        else if (which == StyleVar_WindowPadding) g->style.WindowPadding = v;
        else if (which == StyleVar_ItemSpacing) g->style.ItemSpacing = v;
    }
}

// --- Widgets -----------------------------------------------------------------------

Vec2 CalcTextSize(const char* text, const char* end, bool hideAfterDoubleHash, float wrapWidth) {
    if (!end) end = text + std::strlen(text);
    if (hideAfterDoubleHash)
        if (const char* hash = std::strstr(text, "##"); hash && hash < end) end = hash;
    return g->font.Measure(FontSize(), text, end, wrapWidth > 0.0f ? wrapWidth : 0.0f);
}
void TextUnformatted(const char* text, const char* end) { TextItem(text, end, TextColor(), false); }
void Text(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    const std::string s = Format(fmt, a);
    va_end(a);
    TextItem(s.c_str(), nullptr, TextColor(), false);
}
void TextDisabled(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    const std::string s = Format(fmt, a);
    va_end(a);
    TextItem(s.c_str(), nullptr, DimText(), false);
}
void TextColored(const Vec4& color, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    const std::string s = Format(fmt, a);
    va_end(a);
    TextItem(s.c_str(), nullptr, C(ToColor(color)), false);
}
void TextWrapped(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    const std::string s = Format(fmt, a);
    va_end(a);
    TextItem(s.c_str(), nullptr, TextColor(), true);
}

bool Button(const char* label, Vec2 size) {
    const uint32_t id = IdOf(label);
    const std::string shown = Shown(label);
    const Vec2 text = CalcTextSize(shown.c_str());
    const Vec2 pad = g->style.FramePadding;
    Vec2 s(size.x > 0.0f ? size.x : size.x < 0.0f ? AvailableWidth() + size.x : text.x + pad.x * 2.0f, size.y > 0.0f ? size.y : FrameHeight());
    const Rect rc = ItemAdd(s);
    bool hovered, held;
    const bool pressed = Behaviour(id, rc, hovered, held);
    DL().AddRectFilled(rc.min, rc.max, C(held ? kFrameActive : hovered ? kFrameHover : g->colors[Col_Button]), g->style.FrameRounding);
    if (hovered) DL().AddRect(rc.min, rc.max, C(kAccentDim), g->style.FrameRounding);
    DL().AddText(GetFont(), FontSize(), { std::round(rc.min.x + (s.x - text.x) * 0.5f), std::round(rc.min.y + (s.y - text.y) * 0.5f) }, TextColor(), shown.c_str());
    return pressed;
}
bool SmallButton(const char* label) {
    const Vec2 saved = g->style.FramePadding;
    g->style.FramePadding = { 5.0f, 1.0f };
    const bool pressed = Button(label);
    g->style.FramePadding = saved;
    return pressed;
}
bool InvisibleButton(const char* idText, Vec2 size) {
    const uint32_t id = IdOf(idText);
    const Rect rc = ItemAdd(size);
    bool hovered, held;
    g->lastItemId = id;
    hovered = Hoverable(rc);
    g->lastItemHovered = hovered;
    if (hovered) g->anyHovered = true;
    (void)held;
    return hovered && g->mouseClicked[0];
}
bool Checkbox(const char* label, bool* value) {
    const uint32_t id = IdOf(label);
    const float box = FrameHeight();
    const Rect rc = ItemAdd({ box, box });
    bool hovered, held;
    const bool pressed = Behaviour(id, rc, hovered, held);
    if (pressed) *value = !*value;
    DrawFrame(rc, hovered, held);
    if (*value) {
        const float m = box * 0.25f;
        DL().AddLine({ rc.min.x + m, rc.min.y + box * 0.52f }, { rc.min.x + box * 0.43f, rc.max.y - m }, C(kAccent), 2.5f);
        DL().AddLine({ rc.min.x + box * 0.43f, rc.max.y - m }, { rc.max.x - m, rc.min.y + m }, C(kAccent), 2.5f);
    }
    Label(rc, Shown(label));
    return pressed;
}
namespace {
bool Slider(const char* label, float* value, float min, float max, const std::string& shown, bool integer) {
    const uint32_t id = IdOf(label);
    const Rect rc = ItemAdd({ ItemWidth(), FrameHeight() });
    bool hovered, held;
    Behaviour(id, rc, hovered, held);
    const float before = *value;
    if (held && max > min) {
        const float t = std::clamp((g->io.MousePos.x - rc.min.x - 6.0f) / std::max(1.0f, rc.max.x - rc.min.x - 12.0f), 0.0f, 1.0f);
        *value = min + (max - min) * t;
        if (integer) *value = std::round(*value);
    }
    if (g->navFocus == id && g->navDelta != 0) {
        const float step = integer ? 1.0f : (max - min) / 20.0f;
        *value = std::clamp(*value + step * static_cast<float>(g->navDelta), min, max);
        g->navDelta = 0;
    }
    DrawFrame(rc, hovered, held);
    const float t = max > min ? std::clamp((*value - min) / (max - min), 0.0f, 1.0f) : 0.0f;
    const float gx = rc.min.x + 4.0f + (rc.max.x - rc.min.x - 16.0f) * t;
    DL().AddRectFilled({ rc.min.x + 2.0f, rc.min.y + 2.0f }, { gx + 4.0f, rc.max.y - 2.0f }, C(Rgba(228, 178, 84, 70)), g->style.FrameRounding);
    DL().AddRectFilled({ gx, rc.min.y + 2.0f }, { gx + 8.0f, rc.max.y - 2.0f }, C(held ? kAccent : kAccentDim), 2.0f);
    const Vec2 ts = CalcTextSize(shown.c_str());
    DL().AddText(GetFont(), FontSize(), { std::round((rc.min.x + rc.max.x - ts.x) * 0.5f), rc.min.y + g->style.FramePadding.y }, TextColor(), shown.c_str());
    Label(rc, Shown(label));
    return *value != before;
}
} // namespace
bool SliderFloat(const char* label, float* value, float min, float max, const char* format) {
    char buf[64];
    std::snprintf(buf, sizeof buf, format, *value);
    return Slider(label, value, min, max, buf, false);
}
bool SliderInt(const char* label, int* value, int min, int max, const char* format) {
    float v = static_cast<float>(*value);
    char buf[64];
    std::snprintf(buf, sizeof buf, format, *value);
    const bool changed = Slider(label, &v, static_cast<float>(min), static_cast<float>(max), buf, true);
    *value = static_cast<int>(std::lround(v));
    return changed;
}

namespace {
// A line of text being typed into: `buffer` (size bytes), shown in `frame`.
bool EditText(uint32_t id, const Rect& frame, char* buffer, size_t size, const char* hint, bool enterReturns, bool digitsOnly) {
    bool hovered, held;
    g->lastItemId = id;
    hovered = Hoverable(frame);
    g->lastItemHovered = hovered;
    if (hovered) g->anyHovered = true;
    held = false;
    NavAdd(id, frame);
    bool focused = g->edit.id == id;
    if ((hovered && g->mouseClicked[0]) || (g->navActivate && g->navFocus == id) || (g->focusNextText && !focused)) {
        g->edit.id = id;
        g->edit.text = buffer;
        g->edit.cursor = g->edit.text.size();
        g->clickedAnItem = true;
        g->focusNextText = false;
        if (g->navFocus == id) g->navActivate = false;
        focused = true;
    }
    bool changed = false, entered = false;
    if (focused) {
        TextEdit& e = g->edit;
        if (e.text != buffer) e.text = buffer, e.cursor = e.text.size(); // (changed by the caller: the cursor to its end)
        e.cursor = std::min(e.cursor, e.text.size());
        if (!(g->io.ConfigFlags & ConfigFlags_NoKeyboard)) {
            auto insert = [&](const std::string& s) {
                for (char c : s) {
                    if (e.text.size() + 1 >= size) break;
                    if (digitsOnly && !(std::isdigit(static_cast<unsigned char>(c)) || (c == '-' && e.cursor == 0))) continue;
                    e.text.insert(e.cursor++, 1, c);
                    changed = true;
                }
            };
            for (unsigned c : g->chars) {
                if (c >= 128) continue; // (the font has only ASCII)
                insert(std::string(1, static_cast<char>(c)));
            }
            if (g->ctrlV && g->clipboard)
                if (const char* paste = g->clipboard(g->clipboardUser)) {
                    std::string line(paste);
                    line.erase(std::remove_if(line.begin(), line.end(), [](char c) { return c == '\n' || c == '\r'; }), line.end());
                    insert(line);
                }
            for (Key k : g->keys) {
                if (k == Key::Backspace && e.cursor > 0) e.text.erase(--e.cursor, 1), changed = true;
                else if (k == Key::Delete && e.cursor < e.text.size()) e.text.erase(e.cursor, 1), changed = true;
                else if (k == Key::Left && e.cursor > 0) e.cursor--;
                else if (k == Key::Right && e.cursor < e.text.size()) e.cursor++;
                else if (k == Key::Home) e.cursor = 0;
                else if (k == Key::End) e.cursor = e.text.size();
                else if (k == Key::Enter) entered = true;
                else if (k == Key::Escape) g->edit.id = 0;
            }
            if (changed) std::snprintf(buffer, size, "%s", e.text.c_str());
        }
        g->io.WantTextInput = true;
    }
    // Drawn: the box, its text (or the hint), and the cursor blinking.
    DL().AddRectFilled(frame.min, frame.max, C(focused ? Rgba(30, 32, 40, 245) : hovered ? kFrameHover : kFrame), g->style.FrameRounding);
    if (focused) DL().AddRect(frame.min, frame.max, C(kAccentDim), g->style.FrameRounding);
    DL().PushClipRect({ frame.min.x + 2.0f, frame.min.y }, { frame.max.x - 2.0f, frame.max.y });
    const Vec2 tp(frame.min.x + g->style.FramePadding.x, frame.min.y + g->style.FramePadding.y);
    const std::string& text = focused ? g->edit.text : std::string(buffer);
    float scrollX = 0.0f;
    if (focused) {
        const float caret = CalcTextSize(text.c_str(), text.c_str() + g->edit.cursor).x;
        const float room = frame.max.x - frame.min.x - g->style.FramePadding.x * 2.0f;
        scrollX = std::max(0.0f, caret - room + 4.0f);
    }
    if (text.empty() && hint && !focused) DL().AddText(GetFont(), FontSize(), tp, DimText(), hint);
    else DL().AddText(GetFont(), FontSize(), { tp.x - scrollX, tp.y }, TextColor(), text.c_str());
    if (focused && std::fmod(g->time, 1.0f) < 0.6f) {
        const float caret = tp.x - scrollX + CalcTextSize(text.c_str(), text.c_str() + g->edit.cursor).x;
        DL().AddLine({ caret, frame.min.y + 3.0f }, { caret, frame.max.y - 3.0f }, TextColor(), 1.5f);
    }
    DL().PopClipRect();
    return enterReturns ? entered : changed;
}
} // namespace
bool InputText(const char* label, char* buffer, size_t size, int flags) { return InputTextWithHint(label, nullptr, buffer, size, flags); }
bool InputTextWithHint(const char* label, const char* hint, char* buffer, size_t size, int flags) {
    const uint32_t id = IdOf(label);
    const Rect rc = ItemAdd({ ItemWidth(), FrameHeight() });
    const bool result = EditText(id, rc, buffer, size, hint, (flags & InputTextFlags_EnterReturnsTrue) != 0, false);
    Label(rc, Shown(label));
    return result;
}
bool InputInt(const char* label, int* value) {
    // A box to type the number in, and - and + beside it.
    const uint32_t id = IdOf(label);
    const float total = ItemWidth(), bw = FrameHeight();
    const Rect rc = ItemAdd({ std::max(30.0f, total - bw * 2.0f - 4.0f), FrameHeight() });
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d", *value);
    const int before = *value;
    if (EditText(id, rc, buf, sizeof buf, nullptr, false, true)) *value = std::atoi(buf);
    PushID(label);
    SameLine(0.0f, 2.0f);
    if (Button("-", { bw, bw })) (*value)--;
    SameLine(0.0f, 2.0f);
    if (Button("+", { bw, bw })) (*value)++;
    PopID();
    Label(R().lastItem, Shown(label));
    return *value != before;
}
bool Combo(const char* label, int* current, const char* const items[], int count) {
    const uint32_t id = IdOf(label);
    const Rect rc = ItemAdd({ ItemWidth(), FrameHeight() });
    bool hovered, held;
    const bool pressed = Behaviour(id, rc, hovered, held);
    if (pressed) g->comboOpen = g->comboOpen == id ? 0 : id;
    DrawFrame(rc, hovered || g->comboOpen == id, held);
    const char* shown = *current >= 0 && *current < count ? items[*current] : "";
    DL().PushClipRect(rc.min, { rc.max.x - FrameHeight(), rc.max.y });
    DL().AddText(GetFont(), FontSize(), { rc.min.x + g->style.FramePadding.x, rc.min.y + g->style.FramePadding.y }, TextColor(), shown);
    DL().PopClipRect();
    const float ax = rc.max.x - FrameHeight() * 0.5f, ay = (rc.min.y + rc.max.y) * 0.5f, s = FrameHeight() * 0.18f;
    DL().AddTriangleFilled({ ax - s, ay - s * 0.6f }, { ax + s, ay - s * 0.6f }, { ax, ay + s * 0.7f }, C(kAccent));
    Label(rc, Shown(label));
    bool changed = false;
    if (g->navFocus == id && g->navDelta != 0) {
        *current = std::clamp(*current + g->navDelta, 0, count - 1), changed = true, g->navDelta = 0;
    }
    if (g->comboOpen == id) {
        // The list below it, over everything; a click picks one (or, outside it, closes it).
        const float rowH = LineHeight() + 6.0f;
        float width = rc.max.x - rc.min.x;
        for (int i = 0; i < count; i++) width = std::max(width, CalcTextSize(items[i]).x + 20.0f);
        Rect list{ { rc.min.x, rc.max.y + 2.0f }, { rc.min.x + width, rc.max.y + 2.0f + rowH * count + 4.0f } };
        if (list.max.y > g->viewport.Size.y) list = { { rc.min.x, rc.min.y - 2.0f - rowH * count - 4.0f }, { rc.min.x + width, rc.min.y - 2.0f } };
        g->comboRect = list;
        DrawList& o = g->overlay;
        o.AddRectFilled(list.min, list.max, Rgba(24, 25, 32, 250), 4.0f);
        o.AddRect(list.min, list.max, kAccentDim, 4.0f);
        for (int i = 0; i < count; i++) {
            const Rect row{ { list.min.x + 2.0f, list.min.y + 2.0f + rowH * i }, { list.max.x - 2.0f, list.min.y + 2.0f + rowH * (i + 1) } };
            const bool over = row.Contains(g->io.MousePos);
            if (over || i == *current) o.AddRectFilled(row.min, row.max, over ? kFrameHover : Rgba(60, 50, 30, 200), 3.0f);
            o.AddText(GetFont(), FontSize(), { row.min.x + 6.0f, row.min.y + 3.0f }, g->colors[Col_Text], items[i]);
            if (over && g->mouseClicked[0]) *current = i, changed = true, g->comboOpen = 0, g->clickedAnItem = true;
        }
        if (g->mouseClicked[0] && !list.Contains(g->io.MousePos) && !rc.Contains(g->io.MousePos)) g->comboOpen = 0;
    }
    return changed;
}
void Image(TextureId texture, Vec2 size, Vec2 uv0, Vec2 uv1) {
    const Rect rc = ItemAdd(size);
    DL().AddImage(texture, rc.min, rc.max, uv0, uv1, C(0xFFFFFFFFu));
    g->lastItemId = 0;
    g->lastItemHovered = Hoverable(rc);
}
void SetNextItemOpen(bool open, int cond) { g->nextItemOpenSet = true, g->nextItemOpen = open, g->nextItemOpenCond = cond; }
bool CollapsingHeader(const char* label, int flags) {
    const uint32_t id = IdOf(label);
    auto found = g->opened.find(id);
    if (found == g->opened.end()) found = g->opened.emplace(id, (flags & TreeFlags_DefaultOpen) != 0).first;
    if (g->nextItemOpenSet && (g->nextItemOpenCond == Cond_Always || g->nextItemOpenCond == Cond_Once)) {
        if (g->nextItemOpenCond == Cond_Always || !g->opened.count(id ^ 0xABCDEFu)) found->second = g->nextItemOpen;
        g->opened[id ^ 0xABCDEFu] = true;
    }
    g->nextItemOpenSet = false;
    const Rect rc = ItemAdd({ R().width, FrameHeight() });
    bool hovered, held;
    if (Behaviour(id, rc, hovered, held)) found->second = !found->second;
    DrawFrame(rc, hovered, held, Rgba(52, 56, 72, 220));
    const float s = FrameHeight() * 0.18f, cx = rc.min.x + FrameHeight() * 0.5f, cy = (rc.min.y + rc.max.y) * 0.5f;
    if (found->second) DL().AddTriangleFilled({ cx - s, cy - s * 0.6f }, { cx + s, cy - s * 0.6f }, { cx, cy + s * 0.7f }, C(kAccent));
    else DL().AddTriangleFilled({ cx - s * 0.6f, cy - s }, { cx + s * 0.7f, cy }, { cx - s * 0.6f, cy + s }, C(kAccent));
    DL().AddText(GetFont(), FontSize(), { rc.min.x + FrameHeight(), rc.min.y + g->style.FramePadding.y }, TextColor(), Shown(label).c_str());
    return found->second;
}
void PlotLines(const char* label, const float* values, int count, int offset, const char* overlay, float min, float max, Vec2 size) {
    const Rect rc = ItemAdd({ size.x > 0.0f ? size.x : ItemWidth(), size.y > 0.0f ? size.y : 50.0f });
    DL().AddRectFilled(rc.min, rc.max, C(kFrame), g->style.FrameRounding);
    if (count > 1) {
        float lo = min, hi = max;
        if (lo > 3e38f || hi > 3e38f) {
            lo = 3e38f, hi = -3e38f;
            for (int i = 0; i < count; i++) lo = std::min(lo, values[i]), hi = std::max(hi, values[i]);
        }
        if (hi <= lo) hi = lo + 1.0f;
        std::vector<Vec2> pts;
        for (int i = 0; i < count; i++) {
            const float v = values[(i + offset) % count];
            pts.push_back({ rc.min.x + 3.0f + (rc.max.x - rc.min.x - 6.0f) * i / (count - 1),
                            rc.max.y - 3.0f - (rc.max.y - rc.min.y - 6.0f) * std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) });
        }
        DL().AddPolyline(pts.data(), static_cast<int>(pts.size()), C(kAccent), 1.5f);
    }
    if (overlay) DL().AddText(GetFont(), FontSize(), { rc.min.x + 4.0f, rc.min.y + 2.0f }, TextColor(), overlay);
    Label(rc, Shown(label));
}
void SetKeyboardFocusHere() { g->focusNextText = true; }

bool IsItemHovered() { return g->lastItemHovered; }
bool IsAnyItemHovered() { return g->anyHovered; }
bool IsAnyItemActive() { return g->anyActive || g->edit.id != 0; }
bool IsMouseClicked(int button) { return button >= 0 && button < 3 && g->mouseClicked[button] && !(g->io.ConfigFlags & ConfigFlags_NoMouse); }
bool IsMouseReleased(int button) { return button >= 0 && button < 3 && g->mouseReleased[button] && !(g->io.ConfigFlags & ConfigFlags_NoMouse); }
bool IsKeyPressed(Key key) { return std::find(g->keys.begin(), g->keys.end(), key) != g->keys.end(); }
void SetTooltip(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    g->tooltip = Format(fmt, a);
    va_end(a);
}
void SetItemTooltip(const char* fmt, ...) {
    if (!g->lastItemHovered) return;
    va_list a;
    va_start(a, fmt);
    g->tooltip = Format(fmt, a);
    va_end(a);
}

// --- Tables ------------------------------------------------------------------------

bool BeginTable(const char* idText, int columns, int flags, Vec2 outerSize) {
    Table t;
    t.id = IdOf(idText);
    t.columns = std::max(1, columns);
    t.flags = flags;
    t.state = &g->tables[t.id];
    TableState& s = *t.state;
    if (static_cast<int>(s.measured.size()) != t.columns) s.measured.assign(t.columns, 0.0f), s.fixed.assign(t.columns, 0.0f), s.columnFlags.assign(t.columns, 0);
    t.saved = R();
    t.x = R().cursor.x;
    t.width = outerSize.x > 0.0f ? outerSize.x : AvailableWidth();
    t.top = t.rowTop = t.rowBottom = R().lineTop;
    g->tableStack.push_back(t);
    return true;
}
void TableSetupColumn(const char*, int flags, float width) {
    if (g->tableStack.empty()) return;
    Table& t = g->tableStack.back();
    if (t.setupIndex >= t.columns) return;
    t.state->columnFlags[t.setupIndex] = flags;
    t.state->fixed[t.setupIndex] = (flags & TableColumnFlags_WidthFixed) ? width : 0.0f;
    t.setupIndex++;
}
namespace {
void LayoutColumns(Table& t) {
    TableState& s = *t.state;
    const float pad = g->style.ItemSpacing.x;
    t.colX.assign(t.columns, 0.0f), t.colW.assign(t.columns, 0.0f);
    float fixedTotal = 0.0f;
    int stretch = 0;
    for (int c = 0; c < t.columns; c++) {
        const bool isStretch = (s.columnFlags[c] & TableColumnFlags_WidthStretch) || ((t.flags & TableFlags_SizingStretchProp) && !(s.columnFlags[c] & TableColumnFlags_WidthFixed));
        if (isStretch) stretch++;
        else t.colW[c] = s.fixed[c] > 0.0f ? s.fixed[c] : std::max(8.0f, s.measured[c]), fixedTotal += t.colW[c];
    }
    const float left = std::max(0.0f, t.width - fixedTotal - pad * (t.columns - 1));
    float x = t.x;
    for (int c = 0; c < t.columns; c++) {
        if (t.colW[c] == 0.0f) t.colW[c] = stretch > 0 ? left / stretch : 8.0f;
        t.colX[c] = x;
        x += t.colW[c] + pad;
    }
    for (float& m : s.measured) m = 0.0f;
}
void EndCell(Table& t) {
    if (t.column < 0) return;
    const Region& r = R();
    t.rowBottom = std::max(t.rowBottom, std::max(r.contentMax.y, r.lineTop - g->style.ItemSpacing.y));
    t.state->measured[t.column] = std::max(t.state->measured[t.column], r.contentMax.x - t.colX[t.column]);
}
} // namespace
void TableNextRow() {
    if (g->tableStack.empty()) return;
    Table& t = g->tableStack.back();
    if (t.colX.empty()) LayoutColumns(t);
    EndCell(t);
    if (t.row >= 0) {
        if (static_cast<int>(t.state->rowHeights.size()) <= t.row) t.state->rowHeights.resize(t.row + 1);
        t.state->rowHeights[t.row] = t.rowBottom - t.rowTop;
        t.rowTop = t.rowBottom + 3.0f;
        if (t.flags & TableFlags_BordersInnerH) DL().AddLine({ t.x, t.rowTop - 1.5f }, { t.x + t.width, t.rowTop - 1.5f }, Rgba(255, 255, 255, 30));
    }
    t.row++;
    t.rowBottom = t.rowTop;
    t.column = -1;
    // The row's background (its height from last frame).
    if ((t.flags & TableFlags_RowBg) && t.row % 2 == 1) {
        const float h = t.row < static_cast<int>(t.state->rowHeights.size()) ? t.state->rowHeights[t.row] : FrameHeight();
        DL().AddRectFilled({ t.x - 2.0f, t.rowTop - 1.0f }, { t.x + t.width + 2.0f, t.rowTop + h + 1.0f }, Rgba(255, 255, 255, 12));
    }
}
bool TableNextColumn() {
    if (g->tableStack.empty()) return false;
    Table& t = g->tableStack.back();
    if (t.row < 0 || t.column + 1 >= t.columns) TableNextRow();
    else EndCell(t);
    t.column++;
    Region& r = R();
    r.cursor = { t.colX[t.column], t.rowTop };
    r.lineStartX = t.colX[t.column];
    r.width = t.colW[t.column];
    r.lineTop = r.prevLineTop = t.rowTop;
    r.lineHeight = 0;
    r.contentMax = { t.colX[t.column], t.rowTop };
    return true;
}
void EndTable() {
    if (g->tableStack.empty()) return;
    Table t = g->tableStack.back();
    EndCell(t);
    if (t.row >= 0) {
        if (static_cast<int>(t.state->rowHeights.size()) <= t.row) t.state->rowHeights.resize(t.row + 1);
        t.state->rowHeights[t.row] = t.rowBottom - t.rowTop;
    }
    g->tableStack.pop_back();
    // Back to the area it was in, below the table (as wide as its columns came to).
    Region& r = R();
    float used = 0.0f;
    for (int c = 0; c < static_cast<int>(t.colW.size()); c++) used = std::max(used, t.colX[c] + std::max(t.colW[c], t.state->measured[c]) - t.x);
    r.lineStartX = t.saved.lineStartX;
    r.width = t.saved.width;
    r.cursor = { t.saved.lineStartX, t.top };
    r.lineTop = t.top, r.lineHeight = 0;
    r.contentMax = t.saved.contentMax;
    ItemAdd({ used, std::max(0.0f, t.rowBottom - t.top) });
}

// --- Popups ------------------------------------------------------------------------

void OpenPopup(const char* id) { g->popupsOpen[IdOf(id)] = true; }
bool BeginPopupModal(const char* idText, bool* open, int) {
    const uint32_t id = IdOf(idText);
    if (!g->popupsOpen[id]) return false;
    g->modalNow = true;
    // The world behind dimmed; the popup in the middle of the screen.
    g->overlay.AddRectFilled({ 0, 0 }, g->viewport.Size, Rgba(0, 0, 0, 110));
    SetNextWindowPos({ g->viewport.Size.x * 0.5f, g->viewport.Size.y * 0.5f }, Cond_Always, { 0.5f, 0.5f });
    BeginWindow(Shown(idText).c_str(), open, WindowFlags_AlwaysAutoResize | WindowFlags_NoMove, 1);
    g->popupStack.push_back(id);
    if (open && !*open) g->popupsOpen[id] = false;
    return true;
}
void EndPopup() {
    EndWindow();
    if (!g->popupStack.empty()) g->popupStack.pop_back();
}
void CloseCurrentPopup() {
    if (!g->popupStack.empty()) g->popupsOpen[g->popupStack.back()] = false;
}

// --- Rendering ---------------------------------------------------------------------

void Render(Vec2 framebufferSize) {
    // Clicking off every box takes the keyboard from the one that had it.
    if (g->mouseClicked[0] && !g->clickedAnItem) g->edit.id = 0;
    if (g->edit.id == 0) g->io.WantTextInput = false;
    // The tooltip, by the mouse.
    if (!g->tooltip.empty()) {
        const float wrap = 360.0f;
        const Vec2 size = g->font.Measure(g->io.FontSize, g->tooltip.c_str(), nullptr, wrap);
        Vec2 p = g->io.MousePos + Vec2(16.0f, 14.0f);
        p.x = std::min(p.x, g->viewport.Size.x - size.x - 16.0f);
        p.y = std::min(p.y, g->viewport.Size.y - size.y - 16.0f);
        g->foreground.AddRectFilled(p - Vec2(7, 5), p + size + Vec2(7, 5), Rgba(16, 17, 22, 245), 4.0f);
        g->foreground.AddRect(p - Vec2(7, 5), p + size + Vec2(7, 5), kAccentDim, 4.0f);
        g->foreground.AddText(&g->font, g->io.FontSize, p, Rgba(240, 240, 240), g->tooltip.c_str(), nullptr, wrap);
    }
    // Windows not shown this frame are forgotten for hovering; the order for next frame.
    for (auto& [id, w] : g->windows) w->seenLast = w->seenNow;
    g->lastOrder = g->order;
    std::stable_sort(g->lastOrder.begin(), g->lastOrder.end(), [](const Window* a, const Window* b) { return a->layer < b->layer; });
    g->modalLast = g->modalNow;
    g->navLast = g->navItems;
    if (g->navLast.empty()) g->navVisible = false;
    if (!g->io.MouseDown[0]) g->activeId = 0; // (let go: nothing's held, even if what was isn't shown any more)
    for (bool& c : g->mouseClicked) c = false;
    for (bool& r : g->mouseReleased) r = false;
    g->keys.clear(), g->chars.clear(), g->ctrlV = false;
    g->io.MouseWheel = 0.0f;
    g->io.WantCaptureMouse = g->hoveredWindow != 0;

    // Draw: the background list, the windows (popups over them), the overlay (drop-down
    // lists, a popup's dimming), then the foreground.
    std::vector<const DrawList*> lists{ &g->background };
    for (const Window* w : g->lastOrder)
        if (w->layer == 0 && !w->hidden) lists.push_back(&w->draw);
    lists.push_back(&g->overlay);
    for (const Window* w : g->lastOrder)
        if (w->layer == 1 && !w->hidden) lists.push_back(&w->draw);
    lists.push_back(&g->foreground);

    const Vec2 display = g->io.DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return;
    const float sx = framebufferSize.x / display.x, sy = framebufferSize.y / display.y;
    glViewport(0, 0, static_cast<GLsizei>(framebufferSize.x), static_cast<GLsizei>(framebufferSize.y));
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_SCISSOR_TEST);
    g->shader->Use();
    g->shader->SetUniform("projection", glm::ortho(0.0f, display.x, display.y, 0.0f));
    g->shader->SetUniform("tex", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ebo);
    for (const DrawList* list : lists) {
        if (list->indices.empty()) continue;
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(list->vertices.size() * sizeof(Vertex)), list->vertices.data(), GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(list->indices.size() * sizeof(uint32_t)), list->indices.data(), GL_STREAM_DRAW);
        for (const DrawCmd& cmd : list->commands) {
            if (cmd.indexCount == 0) continue;
            const float x0 = std::max(0.0f, cmd.clip.x), y0 = std::max(0.0f, cmd.clip.y);
            const float x1 = std::min(display.x, cmd.clip.z), y1 = std::min(display.y, cmd.clip.w);
            if (x1 <= x0 || y1 <= y0) continue;
            glScissor(static_cast<GLint>(x0 * sx), static_cast<GLint>((display.y - y1) * sy), static_cast<GLsizei>((x1 - x0) * sx), static_cast<GLsizei>((y1 - y0) * sy));
            glBindTexture(GL_TEXTURE_2D, cmd.texture ? cmd.texture : g->fontTexture);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.indexCount), GL_UNSIGNED_INT, reinterpret_cast<void*>(static_cast<uintptr_t>(cmd.firstIndex * sizeof(uint32_t))));
        }
    }
    glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
}

} // namespace ui
