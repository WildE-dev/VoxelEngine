// The model editor: a program of its own for Resources/Data/creatures.json and
// structures.json (see PlayerModels::CreatureShape for the format), blocks.json (see
// world/BlockData.h) and items.json (see world/Items.h). It shows the model
// being edited as the game draws it, with the part picked flashing, and saves the file
// back both next to the program (so the game, after R, shows it) and in the source tree.
//
//   ModelEditor [creatures|structures|blocks|items] [model, block or item]
//
// Drag (left or right button) to turn round the model, shift-drag or middle-drag to
// move, scroll to zoom. Ctrl (Cmd) S saves, Z undoes, shift Z redoes; Delete removes the
// part picked.
//
// For unattended checks: VOXEL_AUTO_SCREENSHOT="seconds:path.png" saves a screenshot
// that long after starting and quits; VOXEL_EDITOR_PART="0,1" picks a part (by its place
// in the tree), VOXEL_EDITOR_CLICK="x,y" clicks the view there (window coordinates), and
// VOXEL_EDITOR_TOOL (0 move box, 1 move frame, 2 size) picks the handles, and
// VOXEL_EDITOR_ANIM="attack:0.15" shows an animation (held at that time), and
// VOXEL_EDITOR_DO="mirror,history" does those (mirror, copy, paste, history).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "render/PlayerModels.h"
#include "render/Shader.h"
#include "render/TextureAtlas.h"
#include "world/Json.h"

namespace fs = std::filesystem;
using Json::Value;
using Shape = PlayerModels::CreatureShape;
using Part = PlayerModels::ShapePart;

namespace {

// (Each a folder with a file per model, block or item, in Resources/Data.)
constexpr const char* kFiles[] = { "creatures", "structures", "blocks", "items" };
constexpr const char* kWhat[] = { "a creature's model", "a structure's model", "a block", "an item" };
constexpr const char* kFileTitles[] = { "Creatures", "Structures", "Blocks", "Items" };
constexpr int kFileCount = 4;
// Blocks' and items' lines are long: one a line.
int WidthFor(int file) { return file >= 2 ? 300 : 110; }
constexpr const char* kAxes[] = { "x", "y", "z" };
constexpr float kLeftWidth = 270.0f, kRightWidth = 360.0f;

// The order keys are written in, so the files stay easy to read.
int KeyRank(const std::string& k) {
    static const char* order[] = { "name", "inherits", "scale", "colors", "at", "rotate", "swing", "flap", "box", "size", "color", "texture", "tile", "glow", "when", "parts", "animations" };
    for (int i = 0; i < static_cast<int>(std::size(order)); i++)
        if (k == order[i]) return i;
    return 100;
}

void Tidy(Value& v) {
    if (v.IsObject()) {
        std::stable_sort(v.object.begin(), v.object.end(), [](const auto& a, const auto& b) { return KeyRank(a.first) < KeyRank(b.first); });
        for (auto& [k, child] : v.object)
            if (k != "colors" && k != "animations") Tidy(child); // (animations keep the order they're written in)
    } else if (v.IsArray())
        for (auto& child : v.array) Tidy(child);
}

glm::vec3 GetVec(const Value& v, const char* key, glm::vec3 fallback = glm::vec3(0.0f)) {
    const Value* a = v.Find(key);
    if (!a || !a->IsArray()) return fallback;
    glm::vec3 out = fallback;
    for (int i = 0; i < 3 && i < static_cast<int>(a->array.size()); i++)
        if (a->array[static_cast<size_t>(i)].IsNumber()) out[i] = static_cast<float>(a->array[static_cast<size_t>(i)].number);
    return out;
}

void SetVec(Value& v, const char* key, glm::vec3 x) {
    Value a = Json::Array();
    for (int i = 0; i < 3; i++) a.array.push_back(Json::Number(std::round(x[i] * 1000.0) / 1000.0));
    Json::Member(v, key) = a;
}

glm::vec3 FromHex(const std::string& s) {
    unsigned r = 255, g = 255, b = 255;
    if (s.size() == 7 && s[0] == '#') std::sscanf(s.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
    return glm::vec3(r, g, b) / 255.0f;
}

std::string ToHex(glm::vec3 c) {
    char text[8];
    auto byte = [](float f) { return static_cast<unsigned>(std::clamp(std::lround(f * 255.0f), 0L, 255L)); };
    std::snprintf(text, sizeof text, "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    return text;
}

bool IsHex(const std::string& s) { return s.size() == 7 && s[0] == '#'; }

// Every use of colour `from` in some parts renamed `to`.
void RenameColour(Value& parts, const std::string& from, const std::string& to) {
    if (!parts.IsArray()) return;
    for (auto& p : parts.array) {
        if (Value* c = const_cast<Value*>(p.Find("color")); c && c->IsString() && c->string == from) c->string = to;
        if (Value* kids = const_cast<Value*>(p.Find("parts"))) RenameColour(*kids, from, to);
    }
}

void SaveScreenshot(const std::string& path, int width, int height) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1);
    if (stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3)) std::cout << "Saved " << path << std::endl;
    else std::cerr << "Could not save " << path << std::endl;
}

class Editor {
public:
    Editor(TextureAtlas& atlas, PlayerModels& models) : atlas(atlas), models(models) {}

    // Reads a file (from the source tree, if it's there: that's the copy kept).
    void Open(int which) {
        file = which;
        const std::string source = SourcePath(), local = LocalPath();
        const std::string path = !source.empty() && fs::exists(source) ? source : local;
        std::string error;
        Value read;
        if (!Json::ParseDataPath(path, read, error)) {
            status = error;
            read = Json::Object();
        }
        root = read;
        committed = root;
        savedText = Json::Write(root);
        undo.clear();
        redo.clear();
        unsaved = false;
        path_.clear();
        model = root.object.empty() ? "" : root.object.front().first;
        rebuild = true;
        ResetView();
        status = "Opened " + path;
    }

    void Save() {
        Value tidy = root;
        Tidy(tidy);
        std::vector<std::string> wrote;
        // Into a folder: a file per entry (only those that changed are rewritten), and
        // the files of entries gone (deleted or renamed) removed.
        auto put = [&](const std::string& folder) {
            std::error_code ec;
            fs::create_directories(folder, ec);
            bool ok = true;
            for (const auto& [id, value] : tidy.object) {
                const std::string text = "// " + id + ": " + kWhat[file] + " (the format is in README.md, here)\n" + Json::Write(value, WidthFor(file));
                const std::string path = folder + "/" + id + ".json";
                std::ifstream old(path, std::ios::binary);
                const std::string before((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
                if (before == text) continue;
                std::ofstream out(path, std::ios::binary);
                out << text;
                ok = ok && static_cast<bool>(out);
            }
            for (const auto& [id, path] : Json::DataFiles(folder))
                if (!tidy.Find(id)) fs::remove(path, ec);
            if (ok) wrote.push_back(folder);
            else status = "Could not write all of " + folder;
        };
        put(LocalPath());
        const std::string source = SourcePath();
        std::error_code ec;
        if (!source.empty() && fs::exists(fs::path(source).parent_path()) && !fs::equivalent(source, LocalPath(), ec)) put(source);
        if (wrote.empty()) return;
        root = tidy;
        committed = root;
        savedText = Json::Write(root);
        unsaved = false;
        status = "Saved " + wrote.front() + (wrote.size() > 1 ? " and " + wrote.back() : "") + " (a running game loads it again by itself).";
    }

    void Frame(int fbWidth, int fbHeight, int winWidth, int winHeight, float time);

    void Pick(const std::string& m, const std::vector<int>& p) {
        if (root.Find(m)) model = m;
        path_ = p;
        if (!SelectedPart()) path_.clear();
        rebuild = true;
    }
    // (For unattended checks: a click in the view, in window coordinates, and a tool.)
    void ClickAt(float x, float y) { PickAt({ x, y }); }
    void SetTool(int t) { tool = static_cast<Tool>(std::clamp(t, 0, 2)); }
    // (For unattended checks: "mirror", "copy", "paste", "history", comma-separated.)
    void Do(const std::string& what) {
        if (what == "mirror") MirrorPart();
        if (what == "copy") CopyPart();
        if (what == "paste") PastePart();
        if (what == "history") showHistory = true;
        if (what == "save") Save();
    }
    void ShowAnimation(const std::string& name, float at) {
        openAnimTab = true, anim = name;
        if (at >= 0.0f) animNow = at, playing = false;
    }
    void OpenNamed(const std::string& name) {
        for (int i = 0; i < kFileCount; i++)
            if (name == kFiles[i] || name == std::string(kFiles[i]) + ".json") Open(i);
    }

private:
    TextureAtlas& atlas;
    PlayerModels& models;
    int file = 0;
    Value root, committed;
    std::string savedText;
    // History: each step the state before a change, and what the change was.
    struct Step {
        Value state;
        std::string label;
    };
    std::vector<Step> undo, redo;
    std::string pendingLabel;
    bool showHistory = false;
    std::optional<Value> clipboard; // a part copied (Ctrl C), to paste anywhere
    void HistoryWindow();
    void CopyPart();
    void PastePart();
    void MirrorPart();
    std::string DescribeEdit() {
        std::string where = model;
        if (!path_.empty()) {
            const Value* part = SelectedPart();
            const std::string name = part ? part->StringOr("name", "") : "";
            where += name.empty() ? ": part " + std::to_string(path_.back() + 1) : ": " + name;
        }
        return "Changed " + where;
    }
    bool unsaved = false, changed = false, pending = false, rebuild = true;
    std::string model;
    std::vector<int> path_; // the part picked, by place in each list down the tree (empty: none)
    std::string status;
    std::map<std::string, Shape> shapes;
    std::string buildError;
    // The view.
    float yaw = -50.0f, pitch = 20.0f, distance = 4.0f;
    glm::vec3 target{ 0.0f, 0.8f, 0.0f };
    bool walk = false, working = true, flash = true, showGrid = true;
    float sky = 15.0f, block = 0.0f;
    int askFile = -1; // a file to open once the unsaved changes are dealt with
    std::string renameTo, newColour;
    Shader* shader = nullptr;
    // Picking parts in the view, and the handles on the picked one.
    enum class Tool { MoveBox, MoveFrame, Size } tool = Tool::MoveBox;
    std::vector<PlayerModels::PlacedPart> placed;
    glm::mat4 viewMatrix{ 1.0f }, projection{ 1.0f };
    glm::vec4 region{ 0.0f };   // the view, in window coordinates (x, y, width, height)
    glm::vec3 eye{ 0.0f };
    float swingNow = 0.0f;
    bool dragging = false, pressed = false, pressMoved = false;
    int dragAxis = -1, hoverAxis = -1;
    glm::vec2 dragStart{ 0.0f }, dragScreenAxis{ 0.0f }, pressAt{ 0.0f };
    glm::vec3 dragFrom{ 0.0f }, dragFromCentre{ 0.0f };
    struct Handle {
        bool ok = false;
        glm::vec3 origin{ 0.0f };
        glm::vec3 axis[3]{};    // a unit of the value being changed, in the world
    };

public:
    void SetShader(Shader* s) { shader = s; }

private:
    std::string LocalPath() const { return std::string("Resources/Data/") + kFiles[file]; }
    std::string SourcePath() const {
#ifdef VOXEL_SOURCE_DIR
        return std::string(VOXEL_SOURCE_DIR) + "/Resources/Data/" + kFiles[file];
#else
        return "";
#endif
    }
    bool Structures() const { return file == 1; }
    bool Blocks() const { return file == 2; }
    bool ItemsFile() const { return file == 3; }
    // Blocks and items: details to change, not models of parts.
    bool Details() const { return file >= 2; }
    std::string blockFilter;
    // Animations: the one picked (shown while its tab is open), playing or held at a time.
    std::string anim, trackLabel, newAnimName;
    bool animTab = false, playing = true, openAnimTab = false;
    float animNow = 0.0f;
    const PlayerModels::Animation* previewAnim = nullptr;
    void AnimationsTab(Value& m, float time);
    void ResetView() {
        yaw = -50.0f, pitch = 22.0f, distance = Structures() ? 9.0f : Blocks() ? 3.2f : 4.0f;
        target = glm::vec3(0.0f, Structures() ? 1.6f : Blocks() ? 0.5f : 0.8f, 0.0f);
    }

    Value* Model() { return const_cast<Value*>(root.Find(model)); }
    Value& PartsOf(Value& v) {
        Value& parts = Json::Member(v, "parts");
        if (!parts.IsArray()) parts = Json::Array();
        return parts;
    }
    Value* SelectedPart() {
        Value* m = Model();
        if (!m || path_.empty()) return nullptr;
        Value* list = const_cast<Value*>(m->Find("parts"));
        Value* part = nullptr;
        for (int i : path_) {
            if (!list || !list->IsArray() || i < 0 || i >= static_cast<int>(list->array.size())) return nullptr;
            part = &list->array[static_cast<size_t>(i)];
            list = const_cast<Value*>(part->Find("parts"));
        }
        return part;
    }
    // The list the picked part is in.
    Value* SelectedList() {
        Value* m = Model();
        if (!m || path_.empty()) return nullptr;
        Value* list = &PartsOf(*m);
        for (size_t d = 0; d + 1 < path_.size(); d++) list = &PartsOf(list->array[static_cast<size_t>(path_[d])]);
        return list;
    }
    void Changed() {
        changed = pending = rebuild = true;
        if (pendingLabel.empty()) pendingLabel = DescribeEdit();
    }
    void Changed(const std::string& what) {
        changed = pending = rebuild = true;
        pendingLabel = what;
    }

    // The colour names a model can use: its own and those it inherits (own ones first).
    std::vector<std::pair<std::string, bool>> Palette(const std::string& name) {
        std::vector<std::pair<std::string, bool>> out;
        std::string at = name;
        for (int depth = 0; depth < 8 && !at.empty(); depth++) {
            const Value* m = root.Find(at);
            if (!m) break;
            if (const Value* c = m->Find("colors"); c && c->IsObject())
                for (const auto& entry : c->object) {
                    const std::string& k = entry.first;
                    if (std::none_of(out.begin(), out.end(), [&](const auto& e) { return e.first == k; })) out.emplace_back(k, depth == 0);
                }
            at = m->StringOr("inherits", "");
        }
        return out;
    }
    glm::vec3 ColourValue(const std::string& name) {
        if (IsHex(name)) return FromHex(name);
        std::string at = model;
        for (int depth = 0; depth < 8 && !at.empty(); depth++) {
            const Value* m = root.Find(at);
            if (!m) break;
            if (const Value* c = m->Find("colors"); c && c->Find(name)) return FromHex(c->StringOr(name, "#ffffff"));
            at = m->StringOr("inherits", "");
        }
        return glm::vec3(1.0f);
    }

    const Part* HighlightPart() {
        auto it = shapes.find(model);
        if (it == shapes.end() || path_.empty()) return nullptr;
        const Shape& s = it->second;
        size_t first = s.ownStart + static_cast<size_t>(path_[0]);
        if (first >= s.parts.size()) return nullptr;
        const Part* p = &s.parts[first];
        for (size_t d = 1; d < path_.size(); d++) {
            if (static_cast<size_t>(path_[d]) >= p->parts.size()) return nullptr;
            p = &p->parts[static_cast<size_t>(path_[d])];
        }
        return p;
    }

    void Rebuild() {
        rebuild = false;
        shapes.clear();
        buildError.clear();
        if (model.empty()) return;
        if (Blocks()) {
            BuildBlock();
            return;
        }
        if (ItemsFile()) {
            BuildItem();
            return;
        }
        if (!models.BuildShapes(root, { model }, shapes, buildError)) shapes.clear();
    }

    void Undo() {
        if (undo.empty()) return;
        redo.push_back({ committed, undo.back().label });
        root = undo.back().state;
        undo.pop_back();
        AfterHistory();
    }
    void Redo() {
        if (redo.empty()) return;
        undo.push_back({ committed, redo.back().label });
        root = redo.back().state;
        redo.pop_back();
        AfterHistory();
    }
    void AfterHistory() {
        committed = root;
        if (!root.Find(model)) model = root.object.empty() ? "" : root.object.front().first;
        if (!SelectedPart()) path_.clear();
        unsaved = Json::Write(root) != savedText;
        rebuild = true;
    }

    void DrawScene(int x, int y, int w, int h, float time);
    void BuildBlock();
    void BlockInspector(Value& block);
    void BuildItem();
    void ItemInspector(Value& item);
    // A block's textures: top, bottom and the four sides (+x, -x, +z, -z as front, back,
    // left, right), from any of the forms the file allows.
    static void BlockFaces(const Value& block, std::string faces[6]);
    glm::mat4 Base() const { return Structures() ? glm::translate(glm::mat4(1.0f), glm::vec3(-1.5f, 0.0f, -1.5f)) : glm::mat4(1.0f); }
    Handle Handles();
    glm::mat4 HandleMatrix(const Handle& h, int axis, float thickness, bool tip) const;
    bool Ray(glm::vec2 mouse, glm::vec3& origin, glm::vec3& direction) const;
    glm::vec2 ToScreen(glm::vec3 p) const;
    void PickAt(glm::vec2 mouse);
    void ViewInput(bool overView);
    void ViewTools(float top);
    void MenuBar();
    void LeftPanel(float top, float height);
    void RightPanel(float left, float top, float height);
    void PartTree(Value& list, std::vector<int>& prefix);
    void Inspector(Value& part);
    bool TexturePicker(const char* id, std::string& name);
    void UnsavedPrompt();
};

void Editor::Frame(int fbWidth, int fbHeight, int winWidth, int winHeight, float time) {
    changed = false;
    ImGuiIO& io = ImGui::GetIO();
    const float scale = winWidth > 0 ? static_cast<float>(fbWidth) / static_cast<float>(winWidth) : 1.0f;

    // Keys.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) Save();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) Undo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal))
        Redo();
    if (!io.WantTextInput) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteGlobal)) CopyPart();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, ImGuiInputFlags_RouteGlobal)) PastePart();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_M, ImGuiInputFlags_RouteGlobal)) MirrorPart();
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_H, ImGuiInputFlags_RouteGlobal)) showHistory = !showHistory;

    MenuBar();
    const float top = ImGui::GetFrameHeight();
    const float height = static_cast<float>(winHeight) - top;
    LeftPanel(top, height);
    RightPanel(static_cast<float>(winWidth) - kRightWidth, top, height);
    UnsavedPrompt();
    if (showHistory) HistoryWindow();

    // The view between the panels.
    region = glm::vec4(kLeftWidth, top, std::max(1.0f, winWidth - kLeftWidth - kRightWidth), std::max(1.0f, height));
    eye = target + distance * glm::vec3(std::cos(glm::radians(pitch)) * std::cos(glm::radians(yaw)), std::sin(glm::radians(pitch)),
                                        std::cos(glm::radians(pitch)) * std::sin(glm::radians(yaw)));
    viewMatrix = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
    projection = glm::perspective(glm::radians(50.0f), region.z / region.w, 0.05f, 200.0f);
    swingNow = walk ? std::sin(time * 6.0f) * 0.6f : 0.0f;
    if (rebuild) Rebuild();
    placed.clear();
    previewAnim = nullptr;
    if (auto it = shapes.find(model); it != shapes.end()) {
        if (animTab && !anim.empty())
            if (auto a = it->second.animations.find(anim); a != it->second.animations.end()) {
                previewAnim = &a->second;
                // Played over and over, with a breath between (a looping one without).
                if (playing) {
                    const float cycle = a->second.length + (a->second.loop ? 0.0f : 0.4f);
                    animNow = std::min(std::fmod(time, cycle), a->second.length);
                }
            }
        PlayerModels::Place(it->second, Base(), time, swingNow, working, placed, previewAnim, animNow);
    }
    if (!Details()) ViewTools(top);
    const ImVec2 mouse = io.MousePos;
    ViewInput(mouse.x > kLeftWidth && mouse.x < winWidth - kRightWidth && mouse.y > top);
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete) && SelectedPart()) {
        Value* list = SelectedList();
        list->array.erase(list->array.begin() + path_.back());
        path_.pop_back();
        Changed();
    }

    // History: a drag or some typing is one step, made once it's let go.
    if (changed) {
        pending = true;
        rebuild = true;
    }
    if (pending && !ImGui::IsAnyItemActive() && !dragging) {
        undo.push_back({ committed, pendingLabel.empty() ? std::string("Changed") : pendingLabel });
        pendingLabel.clear();
        if (undo.size() > 200) undo.erase(undo.begin());
        redo.clear();
        committed = root;
        pending = false;
        unsaved = Json::Write(root) != savedText;
    }
    if (rebuild) Rebuild();

    const int left = static_cast<int>(kLeftWidth * scale), right = static_cast<int>(kRightWidth * scale);
    DrawScene(left, 0, std::max(1, fbWidth - left - right), std::max(1, fbHeight - static_cast<int>(top * scale)), time);
}

void Editor::CopyPart() {
    if (const Value* part = SelectedPart()) {
        clipboard = *part;
        status = "Copied the part (Ctrl V pastes it, in any model).";
    }
}

void Editor::PastePart() {
    Value* m = Model();
    if (!clipboard || !m || Details()) return;
    if (Value* list = SelectedList(); list && SelectedPart()) {
        list->array.insert(list->array.begin() + path_.back() + 1, *clipboard);
        path_.back()++;
    } else {
        Value& parts = PartsOf(*m);
        parts.array.push_back(*clipboard);
        path_ = { static_cast<int>(parts.array.size()) - 1 };
    }
    Changed("Pasted a part into " + model);
}

namespace {
// A part (and everything in it) mirrored across z = 0: its places' z, its turns about
// x and y, and its swing (so it swings the other way), and left and right in its label.
void Mirror(Value& p) {
    for (const char* key : { "at", "box" })
        if (p.Find(key)) {
            glm::vec3 v = GetVec(p, key);
            v.z = -v.z;
            SetVec(p, key, v);
        }
    if (p.Find("rotate")) {
        glm::vec3 v = GetVec(p, "rotate");
        v.x = -v.x, v.y = -v.y;
        SetVec(p, "rotate", v);
    }
    if (Value* sw = const_cast<Value*>(p.Find("swing")); sw && sw->IsObject()) Json::Member(*sw, "amount") = Json::Number(-sw->NumberOr("amount", 1.0));
    if (Value* name = const_cast<Value*>(p.Find("name")); name && name->IsString()) {
        std::string& n = name->string;
        if (const size_t at = n.find("left"); at != std::string::npos) n.replace(at, 4, "right");
        else if (const size_t at2 = n.find("right"); at2 != std::string::npos) n.replace(at2, 5, "left");
    }
    if (Value* kids = const_cast<Value*>(p.Find("parts")); kids && kids->IsArray())
        for (auto& k : kids->array) Mirror(k);
}
} // namespace

void Editor::MirrorPart() {
    Value* part = SelectedPart();
    if (!part) return;
    Value copy = *part;
    Mirror(copy);
    Value* list = SelectedList();
    list->array.insert(list->array.begin() + path_.back() + 1, copy);
    path_.back()++;
    Changed("Mirrored a part of " + model);
}

void Editor::HistoryWindow() {
    ImGui::SetNextWindowSize(ImVec2(320, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(kLeftWidth + 20.0f, 120.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("History", &showHistory)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Click a step to go back (or forward) to just after it.");
    int goTo = -1; // how many steps to keep undone (undo.size() after)
    if (ImGui::Selectable("(as opened)", undo.empty())) goTo = 0;
    for (size_t i = 0; i < undo.size(); i++)
        if (ImGui::Selectable((undo[i].label + "##u" + std::to_string(i)).c_str(), i + 1 == undo.size())) goTo = static_cast<int>(i + 1);
    for (size_t k = redo.size(); k-- > 0;) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable((redo[k].label + "##r" + std::to_string(k)).c_str(), false)) goTo = static_cast<int>(undo.size() + (redo.size() - k));
        ImGui::PopStyleColor();
    }
    if (goTo >= 0) {
        while (static_cast<int>(undo.size()) > goTo && !undo.empty()) Undo();
        while (static_cast<int>(undo.size()) < goTo && !redo.empty()) Redo();
    }
    ImGui::End();
}

void Editor::DrawScene(int x, int y, int w, int h, float time) {
    glViewport(x, y, w, h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClearColor(0.42f, 0.55f, 0.68f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    const glm::mat4& view = viewMatrix;

    Shader& s = *shader;
    s.Use();
    atlas.Bind(GL_TEXTURE0);
    s.SetUniform("atlas", 0);
    s.SetUniform("fogSky", 1); // (a cube sampler: not on the atlas's unit)
    s.SetUniform("fogEnd", 0.0f);
    s.SetUniform("highlight", 0.0f);
    s.SetUniform("boxRepeat", glm::vec3(0.0f));
    s.SetUniform("cameraPos", eye);
    s.SetUniform("sunDirection", glm::normalize(glm::vec3(0.45f, 1.0f, 0.3f)));
    s.SetUniform("sunBrightness", 1.0f);
    s.SetUniform("blockLightColor", glm::vec3(1.0f, 0.82f, 0.6f));

    // Structures are drawn in their own frame, 0 to 3 across: centred on the view.
    const glm::mat4 base = Base();

    if (showGrid) {
        // A ground grid (whole blocks, for structures: their cells), and which way is +x
        // (the way a creature faces).
        Shape grid;
        auto line = [&](glm::vec3 centre, glm::vec3 size, glm::vec3 colour) {
            Part p;
            p.hasBox = true, p.glow = true;
            p.box = centre, p.size = size, p.color = colour;
            grid.parts.push_back(p);
        };
        const int lo = Structures() ? -2 : -3, hi = Structures() ? 5 : 3;
        const float span = static_cast<float>(hi - lo);
        const float mid = static_cast<float>(lo + hi) * 0.5f;
        for (int i = lo; i <= hi; i++) {
            const bool cell = Structures() && i >= 0 && i <= 3;
            const glm::vec3 c = cell ? glm::vec3(0.85f, 0.75f, 0.35f) : glm::vec3(0.3f, 0.36f, 0.42f);
            line({ static_cast<float>(i), 0.0f, mid }, { 0.012f, 0.004f, span }, c);
            line({ mid, 0.0f, static_cast<float>(i) }, { span, 0.004f, 0.012f }, c);
        }
        const glm::vec3 o = Structures() ? glm::vec3(1.5f, 0.0f, 1.5f) : glm::vec3(0.0f);
        line(o + glm::vec3(0.4f, 0.004f, 0.0f), { 0.8f, 0.01f, 0.025f }, { 0.9f, 0.25f, 0.2f });
        line(o + glm::vec3(0.0f, 0.004f, 0.4f), { 0.025f, 0.01f, 0.8f }, { 0.25f, 0.45f, 0.95f });
        models.DrawPreview(s, view, projection, grid, base, time, 0.0f, false, nullptr, glm::vec2(15.0f, 0.0f));
    }

    auto it = shapes.find(model);
    if (it != shapes.end()) {
        models.DrawPreview(s, view, projection, it->second, base, time, swingNow, working, flash ? HighlightPart() : nullptr, glm::vec2(sky, block), previewAnim,
                           animNow);
    }
    // The handles on the picked part, over everything.
    glDisable(GL_DEPTH_TEST);
    if (const Handle hd = Handles(); hd.ok) {
        std::vector<std::pair<glm::mat4, glm::vec3>> boxes;
        const glm::vec3 colours[3] = { { 0.95f, 0.3f, 0.25f }, { 0.35f, 0.9f, 0.3f }, { 0.3f, 0.5f, 1.0f } };
        for (int i = 0; i < 3; i++) {
            const bool hot = (dragging ? dragAxis : hoverAxis) == i;
            const glm::vec3 c = hot ? glm::vec3(1.0f, 0.9f, 0.3f) : colours[i];
            boxes.emplace_back(HandleMatrix(hd, i, 0.05f, false), c);
            boxes.emplace_back(HandleMatrix(hd, i, 0.0f, true), c);
        }
        models.DrawBoxes(s, view, projection, boxes);
    }
}

Editor::Handle Editor::Handles() {
    Handle h;
    const Part* part = HighlightPart();
    if (!part) return h;
    for (const auto& p : placed) {
        if (p.part != part) continue;
        if (tool == Tool::MoveFrame) {
            // Its frame's origin ("at"), in its parent's frame.
            h.origin = glm::vec3(p.frame[3]);
            for (int i = 0; i < 3; i++) h.axis[i] = glm::vec3(p.parent[i]);
        } else {
            if (!part->hasBox) return h;
            h.origin = glm::vec3(p.frame * glm::vec4(part->box, 1.0f));
            for (int i = 0; i < 3; i++) h.axis[i] = glm::vec3(p.frame[i]);
        }
        h.ok = true;
        for (int i = 0; i < 3; i++)
            if (glm::length(h.axis[i]) < 1e-5f) h.ok = false;
        return h;
    }
    return h;
}

// A handle's shaft (or the cube at its tip), along one axis, sized for the view.
glm::mat4 Editor::HandleMatrix(const Handle& h, int axis, float thickness, bool tip) const {
    const float length = distance * 0.2f;
    const glm::vec3 d = glm::normalize(h.axis[axis]);
    const glm::vec3 u = glm::normalize(glm::cross(d, std::abs(d.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    const glm::vec3 v = glm::cross(d, u);
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(d, 0.0f), m[1] = glm::vec4(u, 0.0f), m[2] = glm::vec4(v, 0.0f);
    if (tip) {
        m[3] = glm::vec4(h.origin + d * length, 1.0f);
        const float size = length * (tool == Tool::Size ? 0.14f : 0.1f) * (thickness > 0.0f ? 2.0f : 1.0f);
        return glm::scale(m, glm::vec3(size));
    }
    m[3] = glm::vec4(h.origin + d * length * 0.5f, 1.0f);
    return glm::scale(m, glm::vec3(length, length * thickness, length * thickness));
}

bool Editor::Ray(glm::vec2 mouse, glm::vec3& origin, glm::vec3& direction) const {
    const float nx = (mouse.x - region.x) / region.z * 2.0f - 1.0f, ny = 1.0f - (mouse.y - region.y) / region.w * 2.0f;
    const glm::mat4 inverse = glm::inverse(projection * viewMatrix);
    glm::vec4 a = inverse * glm::vec4(nx, ny, -1.0f, 1.0f), b = inverse * glm::vec4(nx, ny, 1.0f, 1.0f);
    if (a.w == 0.0f || b.w == 0.0f) return false;
    a /= a.w, b /= b.w;
    origin = glm::vec3(a);
    direction = glm::normalize(glm::vec3(b - a));
    return true;
}

glm::vec2 Editor::ToScreen(glm::vec3 p) const {
    const glm::vec4 clip = projection * viewMatrix * glm::vec4(p, 1.0f);
    const glm::vec3 ndc = glm::vec3(clip) / std::max(clip.w, 1e-4f);
    return { region.x + (ndc.x + 1.0f) * 0.5f * region.z, region.y + (1.0f - ndc.y) * 0.5f * region.w };
}

// Where a ray first meets a unit cube in matrix m (or -1).
float HitBox(const glm::mat4& m, glm::vec3 origin, glm::vec3 direction) {
    const glm::mat4 inverse = glm::inverse(m);
    const glm::vec3 o = glm::vec3(inverse * glm::vec4(origin, 1.0f)), d = glm::vec3(inverse * glm::vec4(direction, 0.0f));
    float near = -1e30f, far = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (std::abs(d[i]) < 1e-9f) {
            if (o[i] < -0.5f || o[i] > 0.5f) return -1.0f;
            continue;
        }
        float t0 = (-0.5f - o[i]) / d[i], t1 = (0.5f - o[i]) / d[i];
        if (t0 > t1) std::swap(t0, t1);
        near = std::max(near, t0), far = std::min(far, t1);
        if (near > far) return -1.0f;
    }
    if (far < 0.0f) return -1.0f;
    return std::max(near, 0.0f);
}

void Editor::PickAt(glm::vec2 mouse) {
    if (Details()) return;
    glm::vec3 o, d;
    if (!Ray(mouse, o, d)) return;
    const Part* best = nullptr;
    float bestT = 1e30f;
    for (const auto& p : placed) {
        if (!p.part->hasBox) continue;
        const float t = HitBox(glm::scale(glm::translate(p.frame, p.part->box), glm::max(p.part->size, glm::vec3(0.002f))), o, d);
        if (t >= 0.0f && t < bestT) bestT = t, best = p.part;
    }
    path_.clear();
    if (!best) return;
    const Shape& shape = shapes[model];
    std::vector<int> found;
    std::function<bool(const std::vector<Part>&)> search = [&](const std::vector<Part>& parts) {
        for (size_t i = 0; i < parts.size(); i++) {
            found.push_back(static_cast<int>(i));
            if (&parts[i] == best || search(parts[i].parts)) return true;
            found.pop_back();
        }
        return false;
    };
    if (!search(shape.parts)) return;
    if (static_cast<size_t>(found[0]) < shape.ownStart) {
        const Value* m = Model();
        status = "That part comes from " + (m ? m->StringOr("inherits", "") : std::string()) + ": change it there.";
        return;
    }
    found[0] -= static_cast<int>(shape.ownStart);
    path_ = found;
}

void Editor::ViewTools(float top) {
    ImGui::SetNextWindowPos(ImVec2(kLeftWidth + 8.0f, top + 8.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("tools", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    int t = static_cast<int>(tool);
    ImGui::RadioButton("Move box (1)", &t, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Move frame (2)", &t, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Size (3)", &t, 2);
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_1)) t = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) t = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) t = 2;
    }
    tool = static_cast<Tool>(t);
    ImGui::TextDisabled("Click a part to pick it. Drag a handle; hold Ctrl to snap to 1/16%s.", tool == Tool::Size ? ", Shift to grow both ways" : "");
    ImGui::End();
}

void Editor::ViewInput(bool overView) {
    ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
    const bool free = overView && !io.WantCaptureMouse;
    const Handle h = Handles();
    hoverAxis = -1;
    if (!dragging && free && h.ok) {
        glm::vec3 o, d;
        float best = 1e30f;
        if (Ray(mouse, o, d))
            for (int i = 0; i < 3; i++)
                for (float t : { HitBox(HandleMatrix(h, i, 0.2f, false), o, d), HitBox(HandleMatrix(h, i, 1.0f, true), o, d) })
                    if (t >= 0.0f && t < best) best = t, hoverAxis = i;
    }

    if (ImGui::IsMouseClicked(0) && free) {
        if (hoverAxis >= 0 && !io.KeyShift && SelectedPart()) {
            dragging = true;
            dragAxis = hoverAxis;
            dragStart = mouse;
            const Value& part = *SelectedPart();
            dragFrom = tool == Tool::MoveFrame ? GetVec(part, "at") : tool == Tool::Size ? GetVec(part, "size", glm::vec3(0.25f)) : GetVec(part, "box");
            dragFromCentre = GetVec(part, "box");
            dragScreenAxis = ToScreen(h.origin + h.axis[dragAxis]) - ToScreen(h.origin);
        } else {
            pressed = true;
            pressMoved = false;
            pressAt = mouse;
        }
    }

    if (dragging) {
        Value* part = SelectedPart();
        if (!io.MouseDown[0] || !part) {
            dragging = false;
            return;
        }
        const float along = glm::dot(dragScreenAxis, dragScreenAxis);
        if (along < 1e-6f) return;
        float moved = glm::dot(mouse - dragStart, dragScreenAxis) / along;
        if (io.KeyCtrl || io.KeySuper) moved = std::round(moved * 16.0f) / 16.0f;
        glm::vec3 v = dragFrom;
        const int i = dragAxis;
        if (tool == Tool::MoveBox) {
            v[i] += moved;
            SetVec(*part, "box", v);
        } else if (tool == Tool::MoveFrame) {
            v[i] += moved;
            SetVec(*part, "at", v);
        } else if (io.KeyShift) {
            v[i] = std::max(0.01f, dragFrom[i] + 2.0f * moved);
            SetVec(*part, "size", v);
            SetVec(*part, "box", dragFromCentre);
        } else {
            // Stretch the face the handle is on; the other stays put.
            v[i] = std::max(0.01f, dragFrom[i] + moved);
            glm::vec3 c = dragFromCentre;
            c[i] += (v[i] - dragFrom[i]) * 0.5f;
            SetVec(*part, "size", v);
            SetVec(*part, "box", c);
        }
        Changed();
        return;
    }

    // Turning round, moving and zooming the view.
    if (pressed || (free && (io.MouseDown[1] || io.MouseDown[2]))) {
        if (glm::length(mouse - pressAt) > 3.0f) pressMoved = true;
        const bool pan = io.MouseDown[2] || ((io.MouseDown[0] || io.MouseDown[1]) && io.KeyShift);
        if (pan) {
            const glm::vec3 forward = glm::normalize(target - eye);
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
            const glm::vec3 up = glm::cross(right, forward);
            target += (-right * io.MouseDelta.x + up * io.MouseDelta.y) * distance * 0.0025f;
        } else if ((pressed && io.MouseDown[0] && pressMoved) || io.MouseDown[1]) {
            yaw += io.MouseDelta.x * 0.4f;
            pitch = std::clamp(pitch + io.MouseDelta.y * 0.4f, -89.0f, 89.0f);
        }
    }
    if (pressed && ImGui::IsMouseReleased(0)) {
        if (!pressMoved) PickAt(mouse);
        pressed = false;
    }
    if (free && io.MouseWheel != 0.0f) distance = std::clamp(distance * std::pow(0.9f, io.MouseWheel), 0.5f, 40.0f);
}

void Editor::MenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        for (int i = 0; i < kFileCount; i++)
            if (ImGui::MenuItem(kFileTitles[i], nullptr, file == i) && i != file) {
                if (unsaved) askFile = i;
                else Open(i);
            }
        ImGui::Separator();
        if (ImGui::MenuItem("Save", "Ctrl+S")) Save();
        if (ImGui::MenuItem("Revert", nullptr, false, unsaved)) Open(file);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo.empty())) Undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !redo.empty())) Redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Copy part", "Ctrl+C", false, SelectedPart() != nullptr)) CopyPart();
        if (ImGui::MenuItem("Paste part", "Ctrl+V", false, clipboard.has_value() && Model() != nullptr && !Details())) PastePart();
        if (ImGui::MenuItem("Mirror part", "Ctrl+M", false, SelectedPart() != nullptr)) MirrorPart();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Flash the picked part", nullptr, &flash);
        ImGui::MenuItem("Ground grid", nullptr, &showGrid);
        ImGui::MenuItem("History", "Ctrl+H", &showHistory);
        if (Structures()) ImGui::MenuItem("Working", nullptr, &working);
        else ImGui::MenuItem("Walking", nullptr, &walk);
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("Sky light", &sky, 0.0f, 15.0f, "%.0f");
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("Block light", &block, 0.0f, 15.0f, "%.0f");
        if (ImGui::MenuItem("Reset the view")) ResetView();
        ImGui::EndMenu();
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s%s", kFiles[file], unsaved ? "  (unsaved)" : "");
    ImGui::Separator();
    ImGui::TextDisabled("%s", status.c_str());
    ImGui::EndMainMenuBar();
}

void Editor::UnsavedPrompt() {
    if (askFile >= 0) ImGui::OpenPopup("Unsaved changes");
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s has changes that aren't saved.", kFiles[file]);
        if (ImGui::Button("Save them")) {
            Save();
            Open(askFile);
            askFile = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Throw them away")) {
            Open(askFile);
            askFile = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            askFile = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void Editor::LeftPanel(float top, float height) {
    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(kLeftWidth, height));
    ImGui::Begin(Blocks() ? "Blocks###left" : ItemsFile() ? "Items###left" : "Models###left", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    for (int i = 0; i < kFileCount; i++) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(kFileTitles[i], file == i) && i != file) {
            if (unsaved) askFile = i;
            else Open(i);
        }
    }

    if (Details()) {
        // Blocks and items are made in the code: their details can be changed, not the list.
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##filter", Blocks() ? "search blocks" : "search items", &blockFilter);
        ImGui::BeginChild("blocks", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (const auto& [name, v] : root.object) {
            const std::string title = v.StringOr("name", name);
            if (!blockFilter.empty() && name.find(blockFilter) == std::string::npos && title.find(blockFilter) == std::string::npos) continue;
            std::string faces[6];
            if (Blocks()) BlockFaces(v, faces);
            else faces[2] = v.StringOr("texture", name);
            const int index = faces[2].empty() ? -1 : atlas.GetIndex(faces[2]);
            if (index > 0) {
                glm::vec2 a, b;
                atlas.GetTileUV(index, a, b);
                ImGui::Image(static_cast<ImTextureID>(atlas.GetTexture()), ImVec2(16, 16), ImVec2(a.x, a.y), ImVec2(b.x, b.y));
            } else
                ImGui::Dummy(ImVec2(16, 16));
            ImGui::SameLine();
            if (ImGui::Selectable((title + "##" + name).c_str(), name == model)) {
                model = name;
                rebuild = true;
            }
        }
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    // The models in the file.
    ImGui::BeginChild("list", ImVec2(0, 190), ImGuiChildFlags_Borders);
    for (const auto& [name, v] : root.object) {
        const std::string inherits = v.StringOr("inherits", "");
        const std::string label = inherits.empty() ? name : name + "  < " + inherits;
        if (ImGui::Selectable(label.c_str(), name == model)) {
            model = name;
            path_.clear();
            rebuild = true;
        }
    }
    ImGui::EndChild();

    if (ImGui::Button("New")) {
        std::string name = "new_model";
        for (int n = 2; root.Find(name); n++) name = "new_model_" + std::to_string(n);
        Value m = Json::Object();
        Value part = Json::Object();
        SetVec(part, "box", { 0.0f, 0.5f, 0.0f });
        SetVec(part, "size", { 0.5f, 1.0f, 0.5f });
        Json::Member(part, "color") = Json::String("#b0b0b0");
        Json::Member(m, "parts") = Json::Array();
        Json::Member(m, "parts").array.push_back(part);
        root.object.emplace_back(name, m);
        model = name;
        path_.clear();
        Changed();
    }
    Value* m = Model();
    if (m) {
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            std::string name = model + "_copy";
            for (int n = 2; root.Find(name); n++) name = model + "_copy" + std::to_string(n);
            root.object.emplace_back(name, *m);
            model = name;
            path_.clear();
            Changed();
            m = Model();
        }
        ImGui::SameLine();
        if (ImGui::Button("Rename")) {
            renameTo = model;
            ImGui::OpenPopup("Rename model");
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete")) ImGui::OpenPopup("Delete model");
    }
    if (ImGui::BeginPopup("Rename model")) {
        ImGui::SetNextItemWidth(200);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", &renameTo, ImGuiInputTextFlags_EnterReturnsTrue);
        if ((enter || ImGui::Button("Rename")) && !renameTo.empty() && !root.Find(renameTo)) {
            for (auto& [k, v] : root.object) {
                if (k == model) k = renameTo;
                if (Value* in = const_cast<Value*>(v.Find("inherits")); in && in->IsString() && in->string == model) in->string = renameTo;
            }
            model = renameTo;
            Changed();
            ImGui::CloseCurrentPopup();
        }
        if (root.Find(renameTo) && renameTo != model) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "There's already a model called that.");
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("Delete model")) {
        ImGui::Text("Delete %s?", model.c_str());
        if (ImGui::Button("Delete it")) {
            root.object.erase(std::remove_if(root.object.begin(), root.object.end(), [&](const auto& e) { return e.first == model; }), root.object.end());
            model = root.object.empty() ? "" : root.object.front().first;
            path_.clear();
            Changed();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    m = Model();
    if (!m) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText(model.c_str());
    // What it inherits from.
    const std::string inherits = m->StringOr("inherits", "");
    if (ImGui::BeginCombo("Inherits", inherits.empty() ? "(nothing)" : inherits.c_str())) {
        if (ImGui::Selectable("(nothing)", inherits.empty())) {
            Json::Remove(*m, "inherits");
            Changed();
        }
        for (const auto& [name, v] : root.object) {
            if (name == model) continue;
            // (Not one that inherits from this, round in a circle.)
            bool loops = false;
            std::string at = name;
            for (int d = 0; d < 8 && !at.empty(); d++) {
                if (at == model) loops = true;
                const Value* x = root.Find(at);
                at = x ? x->StringOr("inherits", "") : "";
            }
            if (loops) continue;
            if (ImGui::Selectable(name.c_str(), name == inherits)) {
                Json::Member(*m, "inherits") = Json::String(name);
                Changed();
            }
        }
        ImGui::EndCombo();
    }
    float scaleValue = static_cast<float>(m->NumberOr("scale", 1.0));
    if (ImGui::DragFloat("Scale", &scaleValue, 0.01f, 0.05f, 10.0f, "%.2f")) {
        if (std::abs(scaleValue - 1.0f) < 1e-4f) Json::Remove(*m, "scale");
        else Json::Member(*m, "scale") = Json::Number(std::round(scaleValue * 1000.0) / 1000.0);
        Changed();
    }

    // Its colours by name (those it inherits shown, but changed in their own model).
    ImGui::SeparatorText("Colours");
    Value& colours = Json::Member(*m, "colors");
    if (!colours.IsObject()) colours = Json::Object();
    for (size_t i = 0; i < colours.object.size(); i++) {
        auto& [name, value] = colours.object[i];
        ImGui::PushID(static_cast<int>(i));
        glm::vec3 c = FromHex(value.string);
        if (ImGui::ColorEdit3("##c", &c.x, ImGuiColorEditFlags_NoInputs)) {
            value = Json::String(ToHex(c));
            Changed();
        }
        ImGui::SameLine();
        std::string edited = name;
        ImGui::SetNextItemWidth(150);
        if (ImGui::InputText("##n", &edited, ImGuiInputTextFlags_EnterReturnsTrue) && !edited.empty() && !IsHex(edited) && !colours.Find(edited)) {
            RenameColour(PartsOf(*m), name, edited);
            name = edited;
            Changed();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            colours.object.erase(colours.object.begin() + static_cast<long>(i));
            Changed();
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    for (const auto& [name, own] : Palette(model)) {
        if (own) continue;
        glm::vec3 c = ColourValue(name);
        ImGui::ColorEdit3(("##inh" + name).c_str(), &c.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoPicker);
        ImGui::SameLine();
        ImGui::TextDisabled("%s (inherited)", name.c_str());
    }
    ImGui::SetNextItemWidth(150);
    const bool add = ImGui::InputTextWithHint("##new", "new colour name", &newColour, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Add") || add) && !newColour.empty() && !IsHex(newColour) && !colours.Find(newColour)) {
        Json::Member(colours, newColour) = Json::String(ToHex(ColourValue(newColour)));
        newColour.clear();
        Changed();
    }
    if (colours.object.empty() && !changed) Json::Remove(*m, "colors");

    if (!buildError.empty()) {
        ImGui::SeparatorText("Problem");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", buildError.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void Editor::RightPanel(float left, float top, float height) {
    ImGui::SetNextWindowPos(ImVec2(left, top));
    ImGui::SetNextWindowSize(ImVec2(kRightWidth, height));
    ImGui::Begin(Blocks() ? "Block###right" : ItemsFile() ? "Item###right" : "Parts###right", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    Value* m = Model();
    if (!m) {
        ImGui::TextDisabled("No model.");
        ImGui::End();
        return;
    }
    if (Blocks()) {
        BlockInspector(*m);
        ImGui::End();
        return;
    }
    if (ItemsFile()) {
        ItemInspector(*m);
        ImGui::End();
        return;
    }
    if (!ImGui::BeginTabBar("right tabs")) {
        ImGui::End();
        return;
    }
    auto animationsTab = [&] {
        bool animOpen = false;
        if (ImGui::BeginTabItem("Animations", nullptr, openAnimTab ? ImGuiTabItemFlags_SetSelected : 0)) {
            animOpen = true;
            AnimationsTab(*m, static_cast<float>(ImGui::GetTime()));
            ImGui::EndTabItem();
        }
        animTab = animOpen;
        openAnimTab = false;
    };
    if (!ImGui::BeginTabItem("Parts")) {
        animationsTab();
        ImGui::EndTabBar();
        ImGui::End();
        return;
    }
    Value& parts = PartsOf(*m);

    auto newPart = [] {
        Value p = Json::Object();
        SetVec(p, "box", { 0.0f, 0.25f, 0.0f });
        SetVec(p, "size", { 0.25f, 0.25f, 0.25f });
        Json::Member(p, "color") = Json::String("#c0c0c0");
        return p;
    };
    Value* picked = SelectedPart();
    if (ImGui::Button("Add")) {
        if (picked) {
            Value* list = SelectedList();
            list->array.insert(list->array.begin() + path_.back() + 1, newPart());
            path_.back()++;
        } else {
            parts.array.push_back(newPart());
            path_ = { static_cast<int>(parts.array.size()) - 1 };
        }
        Changed();
    }
    ImGui::SetItemTooltip("A new part after the one picked");
    ImGui::BeginDisabled(!picked);
    ImGui::SameLine();
    if (ImGui::Button("Add inside")) {
        Value& kids = PartsOf(*picked);
        kids.array.push_back(newPart());
        path_.push_back(static_cast<int>(kids.array.size()) - 1);
        Changed();
    }
    ImGui::SetItemTooltip("A new part in the picked part's frame (moving and turning with it)");
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        Value* list = SelectedList();
        const Value copy = *picked;
        list->array.insert(list->array.begin() + path_.back() + 1, copy);
        path_.back()++;
        Changed("Copied a part of " + model);
    }
    ImGui::SetItemTooltip("A copy of it, after it (Ctrl C copies it to paste anywhere, Ctrl V pastes)");
    ImGui::SameLine();
    if (ImGui::Button("Mirror")) MirrorPart();
    ImGui::SetItemTooltip("A copy of it on the other side (z turned round): a left arm makes a right one (Ctrl M)");
    ImGui::SameLine();
    if (ImGui::ArrowButton("up", ImGuiDir_Up) && path_.back() > 0) {
        Value* list = SelectedList();
        std::swap(list->array[static_cast<size_t>(path_.back())], list->array[static_cast<size_t>(path_.back() - 1)]);
        path_.back()--;
        Changed();
    }
    ImGui::SameLine();
    if (ImGui::ArrowButton("down", ImGuiDir_Down)) {
        Value* list = SelectedList();
        if (path_.back() + 1 < static_cast<int>(list->array.size())) {
            std::swap(list->array[static_cast<size_t>(path_.back())], list->array[static_cast<size_t>(path_.back() + 1)]);
            path_.back()++;
            Changed();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        Value* list = SelectedList();
        list->array.erase(list->array.begin() + path_.back());
        path_.pop_back();
        Changed();
    }
    ImGui::EndDisabled();

    auto it = shapes.find(model);
    if (it != shapes.end() && it->second.ownStart > 0)
        ImGui::TextDisabled("(and %zu parts from %s)", it->second.ownStart, m->StringOr("inherits", "").c_str());

    ImGui::BeginChild("tree", ImVec2(0, height * 0.36f), ImGuiChildFlags_Borders);
    std::vector<int> prefix;
    PartTree(parts, prefix);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && !ImGui::IsAnyItemHovered()) path_.clear();
    ImGui::EndChild();

    if (Value* part = SelectedPart()) {
        ImGui::BeginChild("inspector");
        Inspector(*part);
        ImGui::EndChild();
    } else
        ImGui::TextDisabled("Pick a part to change it.");
    ImGui::EndTabItem();
    animationsTab();
    ImGui::EndTabBar();
    ImGui::End();
}

void Editor::AnimationsTab(Value& m, float) {
    auto it = shapes.find(model);
    if (it == shapes.end()) {
        ImGui::TextDisabled("The model has a problem (see the left).");
        return;
    }
    const Shape& shape = it->second;
    Value* own = const_cast<Value*>(m.Find("animations"));
    auto ownAnim = [&](const std::string& name) -> Value* { return own && own->IsObject() ? const_cast<Value*>(own->Find(name)) : nullptr; };

    // Which: its own, or one it inherits.
    if (!anim.empty() && !shape.animations.count(anim) && !ownAnim(anim)) anim.clear();
    if (anim.empty() && !shape.animations.empty()) anim = shape.animations.begin()->first;
    if (ImGui::BeginCombo("Animation", anim.empty() ? "(none)" : anim.c_str())) {
        for (const auto& [name, a] : shape.animations) {
            const std::string label = name + (ownAnim(name) ? "" : "  (inherited)");
            if (ImGui::Selectable(label.c_str(), name == anim)) anim = name;
        }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(150);
    const bool add = ImGui::InputTextWithHint("##newanim", "attack, work, idle...", &newAnimName, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("New") || add) && !newAnimName.empty() && !ownAnim(newAnimName)) {
        Value& list = Json::Member(m, "animations");
        if (!list.IsObject()) list = Json::Object();
        Value a = Json::Object();
        Json::Member(a, "length") = Json::Number(1.0);
        Json::Member(a, "parts") = Json::Object();
        Json::Member(list, newAnimName) = a;
        anim = newAnimName;
        newAnimName.clear();
        Changed();
        return;
    }
    if (anim.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("No animations. \"attack\" plays as a creature strikes or shoots, \"work\" as a miner digs, and \"idle\" all the time.");
        ImGui::PopTextWrapPos();
        return;
    }
    Value* a = ownAnim(anim);
    if (!a) {
        ImGui::TextDisabled("Inherited from %s.", m.StringOr("inherits", "").c_str());
        if (ImGui::Button("Copy it here to change it")) {
            // From the model it comes from, nearest first.
            std::string at = m.StringOr("inherits", "");
            for (int d = 0; d < 8 && !at.empty(); d++) {
                const Value* from = root.Find(at);
                if (!from) break;
                if (const Value* list = from->Find("animations"); list && list->Find(anim)) {
                    Value& mine = Json::Member(m, "animations");
                    if (!mine.IsObject()) mine = Json::Object();
                    Json::Member(mine, anim) = *list->Find(anim);
                    Changed();
                    break;
                }
                at = from->StringOr("inherits", "");
            }
        }
    } else {
        ImGui::SameLine();
        if (ImGui::Button("Delete")) {
            Json::Remove(*own, anim);
            anim.clear();
            Changed();
            return;
        }
    }

    const auto& built = shape.animations.find(anim);
    const float length = built != shape.animations.end() ? built->second.length : 1.0f;
    if (a) {
        float len = static_cast<float>(a->NumberOr("length", 1.0));
        if (ImGui::DragFloat("Length", &len, 0.01f, 0.05f, 30.0f, "%.2f s")) {
            Json::Member(*a, "length") = Json::Number(std::round(std::max(0.05f, len) * 100.0) / 100.0);
            Changed();
        }
        bool loop = a->BoolOr("loop", false);
        if (ImGui::Checkbox("Loops", &loop)) {
            if (loop) Json::Member(*a, "loop") = Json::Bool(true);
            else Json::Remove(*a, "loop");
            Changed();
        }
    }
    ImGui::Checkbox("Play", &playing);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##time", &animNow, 0.0f, length, "%.2f s")) playing = false;

    // The labelled parts (its own and inherited): each one's keys.
    ImGui::SeparatorText("Parts it moves");
    std::vector<std::string> labels;
    std::function<void(const std::vector<Part>&)> collect = [&](const std::vector<Part>& parts) {
        for (const auto& p : parts) {
            if (!p.name.empty() && std::find(labels.begin(), labels.end(), p.name) == labels.end()) labels.push_back(p.name);
            collect(p.parts);
        }
    };
    collect(shape.parts);
    if (labels.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("No part has a label: give one a label (on the Parts tab) to animate it.");
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::BeginChild("labels", ImVec2(0, 120), ImGuiChildFlags_Borders);
    for (const auto& l : labels) {
        size_t keys = 0;
        if (built != shape.animations.end())
            if (auto t = built->second.tracks.find(l); t != built->second.tracks.end()) keys = t->second.size();
        const std::string text = l + (keys ? "  (" + std::to_string(keys) + " keys)" : "");
        if (ImGui::Selectable(text.c_str(), l == trackLabel)) trackLabel = l;
    }
    ImGui::EndChild();
    if (trackLabel.empty() || !a) return;

    Value& tracks = Json::Member(*a, "parts");
    if (!tracks.IsObject()) tracks = Json::Object();
    Value* keys = const_cast<Value*>(tracks.Find(trackLabel));
    ImGui::SeparatorText(trackLabel.c_str());
    if (ImGui::Button("Add a key here")) {
        // Where it is now in the animation, to change from.
        glm::vec3 rotate(0.0f), move(0.0f);
        if (built != shape.animations.end()) PlayerModels::Sample(built->second, trackLabel, animNow, rotate, move);
        Value k = Json::Object();
        Json::Member(k, "t") = Json::Number(std::round(animNow * 100.0) / 100.0);
        SetVec(k, "rotate", rotate);
        if (move != glm::vec3(0.0f)) SetVec(k, "move", move);
        Value& list = Json::Member(tracks, trackLabel);
        if (!list.IsArray()) list = Json::Array();
        size_t at = 0;
        while (at < list.array.size() && list.array[at].NumberOr("t", 0.0) <= animNow) at++;
        list.array.insert(list.array.begin() + static_cast<long>(at), k);
        playing = false;
        Changed();
        return;
    }
    if (!keys || !keys->IsArray()) return;
    for (size_t i = 0; i < keys->array.size(); i++) {
        Value& k = keys->array[i];
        ImGui::PushID(static_cast<int>(i));
        float t = static_cast<float>(k.NumberOr("t", 0.0));
        ImGui::SetNextItemWidth(70);
        if (ImGui::DragFloat("##t", &t, 0.01f, 0.0f, length, "%.2f s")) {
            Json::Member(k, "t") = Json::Number(std::round(std::clamp(t, 0.0f, 30.0f) * 100.0) / 100.0);
            animNow = t, playing = false;
            Changed();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("go")) animNow = t, playing = false;
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            keys->array.erase(keys->array.begin() + static_cast<long>(i));
            if (keys->array.empty()) Json::Remove(tracks, trackLabel);
            Changed();
            ImGui::PopID();
            break;
        }
        glm::vec3 rotate = GetVec(k, "rotate"), move = GetVec(k, "move");
        if (ImGui::DragFloat3("Turn", &rotate.x, 0.5f, 0.0f, 0.0f, "%.1f")) {
            SetVec(k, "rotate", rotate);
            animNow = t, playing = false;
            Changed();
        }
        if (ImGui::DragFloat3("Move", &move.x, 0.005f, 0.0f, 0.0f, "%.3f")) {
            if (move == glm::vec3(0.0f)) Json::Remove(k, "move");
            else SetVec(k, "move", move);
            animNow = t, playing = false;
            Changed();
        }
        ImGui::Separator();
        ImGui::PopID();
    }
}

void Editor::PartTree(Value& list, std::vector<int>& prefix) {
    for (size_t i = 0; i < list.array.size(); i++) {
        Value& p = list.array[i];
        prefix.push_back(static_cast<int>(i));
        const Value* kids = p.Find("parts");
        const bool hasKids = kids && kids->IsArray() && !kids->array.empty();
        std::string label = p.StringOr("name", "");
        if (label.empty()) {
            if (p.Find("size")) {
                const glm::vec3 s = GetVec(p, "size");
                char text[64];
                std::snprintf(text, sizeof text, "box %.2g x %.2g x %.2g", s.x, s.y, s.z);
                label = text;
            } else
                label = "frame";
        }
        if (p.Find("texture")) label += "  [textured]";
        if (const std::string when = p.StringOr("when", ""); !when.empty()) label += "  (" + when + ")";
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
        if (!hasKids) flags |= ImGuiTreeNodeFlags_Leaf;
        if (prefix == path_) flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::PushID(static_cast<int>(i));
        // A swatch of its colour.
        if (p.Find("size")) {
            const glm::vec3 c = ColourValue(p.StringOr("color", "#ffffff"));
            ImGui::ColorButton("##sw", ImVec4(c.r, c.g, c.b, 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(10, ImGui::GetFontSize()));
            ImGui::SameLine();
        }
        const bool open = ImGui::TreeNodeEx("##part", flags, "%s", label.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) path_ = prefix;
        if (open) {
            if (hasKids) PartTree(*const_cast<Value*>(kids), prefix);
            ImGui::TreePop();
        }
        ImGui::PopID();
        prefix.pop_back();
    }
}

bool Editor::TexturePicker(const char* id, std::string& name) {
    bool picked = false;
    ImGui::PushID(id);
    const int index = name.empty() ? -1 : atlas.GetIndex(name);
    glm::vec2 a, b;
    const ImTextureID texture = static_cast<ImTextureID>(atlas.GetTexture());
    if (index >= 0) {
        atlas.GetTileUV(index, a, b);
        ImGui::Image(texture, ImVec2(20, 20), ImVec2(a.x, a.y), ImVec2(b.x, b.y));
        ImGui::SameLine();
    }
    if (ImGui::Button(name.empty() ? "(none)" : name.c_str(), ImVec2(180, 0))) ImGui::OpenPopup("textures");
    ImGui::SameLine();
    ImGui::TextUnformatted(id);
    if (ImGui::BeginPopup("textures")) {
        static std::string filter;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(300);
        ImGui::InputTextWithHint("##filter", "search", &filter);
        ImGui::BeginChild("grid", ImVec2(330, 300));
        int shown = 0;
        const auto& names = atlas.GetNames();
        for (size_t i = 1; i < names.size(); i++) {
            if (!filter.empty() && names[i].find(filter) == std::string::npos) continue;
            atlas.GetTileUV(static_cast<int>(i), a, b);
            if (shown % 7 != 0) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            const bool on = names[i] == name;
            if (ImGui::ImageButton("t", texture, ImVec2(34, 34), ImVec2(a.x, a.y), ImVec2(b.x, b.y), on ? ImVec4(1, 0.8f, 0.2f, 1) : ImVec4(0, 0, 0, 0))) {
                name = names[i];
                picked = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SetItemTooltip("%s", names[i].c_str());
            ImGui::PopID();
            shown++;
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return picked;
}

void Editor::BlockFaces(const Value& block, std::string faces[6]) {
    const Value* t = block.Find("textures");
    if (!t || !t->IsObject()) return;
    const std::string all = t->StringOr("all", "");
    const std::string side = t->StringOr("side", all);
    faces[0] = t->StringOr("top", all);
    faces[1] = t->StringOr("bottom", all);
    const char* sides[4] = { "front", "back", "left", "right" };
    for (int i = 0; i < 4; i++) faces[2 + i] = t->StringOr(sides[i], side);
}

void Editor::BuildBlock() {
    const Value* block = Model();
    if (!block) return;
    std::string faces[6];
    BlockFaces(*block, faces);
    Part p;
    p.hasBox = true;
    p.box = glm::vec3(0.0f, 0.5f, 0.0f);
    p.size = glm::vec3(1.0f);
    p.color = glm::vec3(1.0f);
    p.glow = block->NumberOr("light", 0.0) > 0.0;
    // (Drawn as a model box: top, bottom, and one texture for its sides.)
    const std::string use[3] = { faces[0], faces[1], faces[2] };
    for (int f = 0; f < 3; f++) {
        if (use[f].empty()) continue;
        glm::vec2 a, b;
        atlas.GetTileUV(atlas.GetIndex(use[f]), a, b);
        p.uv[f] = glm::vec4(a, b);
        p.textured = true;
    }
    if (!p.textured) p.color = FromHex(block->StringOr("map", "#b0b0b0"));
    Shape shape;
    shape.parts.push_back(p);
    shapes[model] = shape;
}

void Editor::BlockInspector(Value& block) {
    ImGui::SeparatorText(model.c_str());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("What it is and does stays in the code; these details can be changed. A field left out keeps the game's own.");
    ImGui::PopTextWrapPos();
    std::string name = block.StringOr("name", "");
    if (ImGui::InputText("Name", &name)) {
        Json::Member(block, "name") = Json::String(name);
        Changed();
    }

    ImGui::SeparatorText("Textures");
    std::string faces[6];
    BlockFaces(block, faces);
    const Value* t = block.Find("textures");
    int mode = 0;
    if (t && t->IsObject()) {
        if (t->Find("front") || t->Find("back") || t->Find("left") || t->Find("right")) mode = 2;
        else if (t->Find("top") || t->Find("bottom") || t->Find("side")) mode = 1;
    }
    const char* modes[] = { "One for every face", "Top, bottom, sides", "Each face" };
    bool changedTextures = ImGui::Combo("Faces", &mode, modes, 3);
    if (mode == 0) changedTextures |= TexturePicker("Every face", faces[2]), faces[0] = faces[1] = faces[2];
    else if (mode == 1) {
        changedTextures |= TexturePicker("Top", faces[0]);
        changedTextures |= TexturePicker("Bottom", faces[1]);
        changedTextures |= TexturePicker("Sides", faces[2]);
    } else {
        const char* labels[6] = { "Top", "Bottom", "Front", "Back", "Left", "Right" };
        for (int i = 0; i < 6; i++) changedTextures |= TexturePicker(labels[i], faces[i]);
        ImGui::TextDisabled("(The preview shows the front on every side.)");
    }
    if (changedTextures) {
        Value o = Json::Object();
        if (mode == 0) Json::Member(o, "all") = Json::String(faces[2]);
        else if (mode == 1) {
            Json::Member(o, "top") = Json::String(faces[0]);
            Json::Member(o, "bottom") = Json::String(faces[1]);
            Json::Member(o, "side") = Json::String(faces[2]);
        } else {
            const char* keys[6] = { "top", "bottom", "front", "back", "left", "right" };
            for (int i = 0; i < 6; i++) Json::Member(o, keys[i]) = Json::String(faces[i]);
        }
        Json::Member(block, "textures") = o;
        Changed();
    }

    ImGui::SeparatorText("Breaking");
    float hardness = static_cast<float>(block.NumberOr("hardness", 1.0));
    if (ImGui::DragFloat("Hardness", &hardness, 0.05f, 0.0f, 100.0f, "%.2f s")) {
        Json::Member(block, "hardness") = Json::Number(std::round(std::max(0.0f, hardness) * 100.0) / 100.0);
        Changed();
    }
    ImGui::SetItemTooltip("Seconds to break it by hand");
    auto choose = [&](const char* key, const char* label, std::initializer_list<const char*> options) {
        const std::string now = block.StringOr(key, "");
        if (ImGui::BeginCombo(label, now.c_str())) {
            for (const char* o : options)
                if (ImGui::Selectable(o, now == o)) {
                    Json::Member(block, key) = Json::String(o);
                    Changed();
                }
            ImGui::EndCombo();
        }
    };
    choose("tool", "Best tool", { "pickaxe", "axe", "shovel", "sword", "none" });
    choose("sound", "Sound", { "soft", "hard", "sand", "wood", "glass" });

    ImGui::SeparatorText("Looks");
    int light = static_cast<int>(block.NumberOr("light", 0.0));
    if (ImGui::SliderInt("Light", &light, 0, 15)) {
        Json::Member(block, "light") = Json::Number(light);
        Changed();
    }
    glm::vec3 map = FromHex(block.StringOr("map", "#707070"));
    if (ImGui::ColorEdit3("Map colour", &map.x)) {
        Json::Member(block, "map") = Json::String(ToHex(map));
        Changed();
    }
}

void Editor::BuildItem() {
    // (Drawn as a thin card showing its icon, as it's held.)
    const Value* item = Model();
    if (!item) return;
    Part p;
    p.hasBox = true;
    p.box = glm::vec3(0.0f, 0.5f, 0.0f);
    p.size = glm::vec3(0.8f, 0.8f, 0.05f);
    p.color = glm::vec3(1.0f);
    const std::string texture = item->StringOr("texture", model);
    glm::vec2 a, b;
    atlas.GetTileUV(atlas.GetIndex(texture), a, b);
    for (auto& uv : p.uv) uv = glm::vec4(a, b);
    p.textured = true;
    Shape shape;
    shape.parts.push_back(p);
    shapes[model] = shape;
}

void Editor::ItemInspector(Value& item) {
    ImGui::SeparatorText(model.c_str());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("An item: carried and used, never placed as a block. What it is and does stays in the code; these details can be changed. A field left out keeps the game's own.");
    ImGui::PopTextWrapPos();
    std::string name = item.StringOr("name", "");
    if (ImGui::InputText("Name", &name)) {
        Json::Member(item, "name") = Json::String(name);
        Changed();
    }
    std::string texture = item.StringOr("texture", "");
    if (TexturePicker("Icon", texture)) {
        Json::Member(item, "texture") = Json::String(texture);
        Changed();
    }
}

void Editor::Inspector(Value& part) {
    std::string name = part.StringOr("name", "");
    if (ImGui::InputTextWithHint("Label", "(just for the editor)", &name)) {
        if (name.empty()) Json::Remove(part, "name");
        else Json::Member(part, "name") = Json::String(name);
        Changed();
    }

    auto vec = [&](const char* key, const char* label, float speed, bool dropZero, const char* tip) {
        glm::vec3 v = GetVec(part, key);
        if (ImGui::DragFloat3(label, &v.x, speed, 0.0f, 0.0f, "%.3f")) {
            if (dropZero && v == glm::vec3(0.0f)) Json::Remove(part, key);
            else SetVec(part, key, v);
            Changed();
        }
        ImGui::SetItemTooltip("%s", tip);
    };

    ImGui::SeparatorText("Frame");
    vec("at", "At", 0.01f, true, "Where this part's frame is in its parent's (a limb's pivot)");
    vec("rotate", "Rotate", 0.5f, true, "Degrees about x, then y, then z");

    ImGui::SeparatorText("Box");
    bool hasBox = part.Find("size") != nullptr;
    if (ImGui::Checkbox("Has a box", &hasBox)) {
        if (hasBox) {
            SetVec(part, "size", glm::vec3(0.25f));
            Json::Member(part, "color") = Json::String("#c0c0c0");
        } else
            for (const char* k : { "box", "size", "color", "texture", "tile", "glow" }) Json::Remove(part, k);
        Changed();
    }
    if (hasBox) {
        vec("box", "Centre", 0.01f, true, "The box's centre, in this part's frame");
        glm::vec3 size = GetVec(part, "size");
        if (ImGui::DragFloat3("Size", &size.x, 0.01f, 0.001f, 64.0f, "%.3f")) {
            SetVec(part, "size", glm::max(size, glm::vec3(0.001f)));
            Changed();
        }

        // Its colour: one of the model's named ones, or its own.
        std::string colour = part.StringOr("color", "#ffffff");
        glm::vec3 c = ColourValue(colour);
        if (ImGui::ColorEdit3("##colour", &c.x, ImGuiColorEditFlags_NoInputs)) {
            if (IsHex(colour)) Json::Member(part, "color") = Json::String(ToHex(c));
            else {
                // Changing a named colour changes it for every part using it.
                Value* m = Model();
                Value& colours = Json::Member(*m, "colors");
                if (!colours.IsObject()) colours = Json::Object();
                Json::Member(colours, colour) = Json::String(ToHex(c));
            }
            Changed();
        }
        ImGui::SameLine();
        if (ImGui::BeginCombo("Colour", IsHex(colour) ? "(its own)" : colour.c_str())) {
            if (ImGui::Selectable("(its own)", IsHex(colour))) {
                Json::Member(part, "color") = Json::String(ToHex(c));
                Changed();
            }
            for (const auto& [n, own] : Palette(model)) {
                const glm::vec3 pc = ColourValue(n);
                ImGui::ColorButton(("##" + n).c_str(), ImVec4(pc.r, pc.g, pc.b, 1), ImGuiColorEditFlags_NoTooltip, ImVec2(12, 12));
                ImGui::SameLine();
                if (ImGui::Selectable(n.c_str(), n == colour)) {
                    Json::Member(part, "color") = Json::String(n);
                    Changed();
                }
            }
            ImGui::EndCombo();
        }

        // Its texture, from the block textures: none, one for all faces, or top,
        // bottom and sides. With one, the colour tints it.
        ImGui::SeparatorText("Texture");
        std::string faces[3];
        const Value* tex = part.Find("texture");
        int mode = 0;
        if (tex && tex->IsString()) {
            mode = 1;
            faces[0] = faces[1] = faces[2] = tex->string;
        } else if (tex && tex->IsObject()) {
            mode = 2;
            faces[2] = tex->StringOr("side", tex->StringOr("all", ""));
            faces[0] = tex->StringOr("top", faces[2]);
            faces[1] = tex->StringOr("bottom", faces[2]);
        }
        const char* modes[] = { "None", "One for every face", "Top, bottom, sides" };
        int newMode = mode;
        bool texChanged = false;
        if (ImGui::Combo("Texture", &newMode, modes, 3) && newMode != mode) {
            if (newMode > 0 && faces[2].empty()) faces[0] = faces[1] = faces[2] = atlas.GetNames().size() > 1 ? atlas.GetNames()[1] : "";
            if (newMode == 1) faces[0] = faces[1] = faces[2];
            mode = newMode;
            texChanged = true;
            // A texture shows as itself under a white tint.
            if (mode > 0 && IsHex(colour) && !tex) Json::Member(part, "color") = Json::String("#ffffff");
        }
        if (mode == 1) texChanged |= TexturePicker("Every face", faces[2]), faces[0] = faces[1] = faces[2];
        if (mode == 2) {
            texChanged |= TexturePicker("Top", faces[0]);
            texChanged |= TexturePicker("Bottom", faces[1]);
            texChanged |= TexturePicker("Sides", faces[2]);
        }
        if (texChanged) {
            if (mode == 0) {
                Json::Remove(part, "texture");
                Json::Remove(part, "tile");
            } else if (mode == 1)
                Json::Member(part, "texture") = Json::String(faces[2]);
            else {
                Value o = Json::Object();
                Json::Member(o, "top") = Json::String(faces[0]);
                Json::Member(o, "bottom") = Json::String(faces[1]);
                Json::Member(o, "side") = Json::String(faces[2]);
                Json::Member(part, "texture") = o;
            }
            Changed();
        }
        if (mode > 0) {
            bool tile = part.BoolOr("tile", false);
            if (ImGui::Checkbox("Repeat it once a block", &tile)) {
                if (tile) Json::Member(part, "tile") = Json::Bool(true);
                else Json::Remove(part, "tile");
                Changed();
            }
            ImGui::SetItemTooltip("Otherwise the texture is stretched over each face once");
        }
        bool glow = part.BoolOr("glow", false);
        if (ImGui::Checkbox("Glows", &glow)) {
            if (glow) Json::Member(part, "glow") = Json::Bool(true);
            else Json::Remove(part, "glow");
            Changed();
        }
        ImGui::SetItemTooltip("Lit by itself (a lamp, eyes, fire)");
    }

    ImGui::SeparatorText("Shown");
    const std::string when = part.StringOr("when", "");
    const char* whens[] = { "always", "tame", "working", "idle" };
    int w = 0;
    for (int i = 1; i < 4; i++)
        if (when == whens[i]) w = i;
    if (ImGui::Combo("When", &w, whens, 4)) {
        if (w == 0) Json::Remove(part, "when");
        else Json::Member(part, "when") = Json::String(whens[w]);
        Changed();
    }
    ImGui::SetItemTooltip("tame: a tame creature's (a wolf's collar); working / idle: a structure's, while it works or doesn't");

    ImGui::SeparatorText("Motion");
    auto axisCombo = [&](Value& m, const char* fallback) {
        int axis = 0;
        const std::string a = m.StringOr("axis", fallback);
        for (int i = 0; i < 3; i++)
            if (a == kAxes[i]) axis = i;
        ImGui::SetNextItemWidth(60);
        if (ImGui::Combo("Axis", &axis, kAxes, 3)) {
            Json::Member(m, "axis") = Json::String(kAxes[axis]);
            Changed();
        }
    };
    auto number = [&](Value& m, const char* key, const char* label, double fallback, float speed) {
        float v = static_cast<float>(m.NumberOr(key, fallback));
        ImGui::SetNextItemWidth(110);
        if (ImGui::DragFloat(label, &v, speed, 0.0f, 0.0f, "%.2f")) {
            Json::Member(m, key) = Json::Number(std::round(v * 100.0) / 100.0);
            Changed();
        }
    };
    bool swing = part.Find("swing") != nullptr;
    if ((!Structures() || swing) && ImGui::Checkbox("Swings as it walks", &swing)) {
        if (swing) {
            Value s = Json::Object();
            Json::Member(s, "axis") = Json::String("z");
            Json::Member(s, "amount") = Json::Number(1);
            Json::Member(part, "swing") = s;
            if (!Structures()) walk = true;
        } else
            Json::Remove(part, "swing");
        Changed();
    }
    if (Value* s = const_cast<Value*>(part.Find("swing"))) {
        ImGui::PushID("swing");
        ImGui::Indent();
        axisCombo(*s, "z");
        ImGui::SameLine();
        number(*s, "amount", "Amount", 1.0, 0.05f);
        ImGui::SetItemTooltip("Negative to swing the other way (the other legs)");
        ImGui::Unindent();
        ImGui::PopID();
    }
    bool flap = part.Find("flap") != nullptr;
    if (ImGui::Checkbox(Structures() ? "Moves while it works" : "Flaps all the time", &flap)) {
        if (flap) {
            Value f = Json::Object();
            Json::Member(f, "axis") = Json::String("x");
            Json::Member(f, "speed") = Json::Number(10);
            Json::Member(f, "amount") = Json::Number(0.5);
            Json::Member(part, "flap") = f;
        } else
            Json::Remove(part, "flap");
        Changed();
    }
    if (Value* f = const_cast<Value*>(part.Find("flap"))) {
        ImGui::PushID("flap");
        ImGui::Indent();
        axisCombo(*f, "x");
        number(*f, "speed", "Speed", 10.0, 0.1f);
        ImGui::SameLine();
        number(*f, "amount", "Amount", 0.5, 0.01f);
        ImGui::Unindent();
        ImGui::PopID();
    }
}

void GlfwError(int code, const char* description) { std::cerr << "GLFW error " << code << ": " << description << std::endl; }

} // namespace

int main(int argc, char** argv) {
    // Run from anywhere: the resources are next to the program.
    if (!fs::exists("Resources") && argc > 0) {
        std::error_code ec;
        const fs::path dir = fs::absolute(argv[0], ec).parent_path();
        if (fs::exists(dir / "Resources")) fs::current_path(dir, ec);
    }

    glfwSetErrorCallback(GlfwError);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(1440, 860, "Model editor", nullptr, nullptr);
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    if (!gladLoadGL()) return 1;
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    int result = 0;
    {
        TextureAtlas atlas;
        atlas.Build("Resources/Textures/Blocks");
        // (The shader's fog samples a cube map, unused here: one to sample all the same.)
        GLuint cube = 0;
        glGenTextures(1, &cube);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
        const unsigned char grey[3] = { 128, 128, 128 };
        for (int f = 0; f < 6; f++) glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, grey);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glActiveTexture(GL_TEXTURE0);
        Shader shader("Resources/Shaders/player.vert", "Resources/Shaders/player.frag");
        PlayerModels models;
        models.SetAtlas(&atlas);
        Editor editor(atlas, models);
        editor.SetShader(&shader);
        editor.Open(0);
        if (argc > 1) editor.OpenNamed(argv[1]);
        std::vector<int> part;
        if (const char* p = std::getenv("VOXEL_EDITOR_PART")) {
            std::stringstream in(p);
            std::string n;
            while (std::getline(in, n, ',')) part.push_back(std::atoi(n.c_str()));
        }
        if (argc > 2 || !part.empty()) editor.Pick(argc > 2 ? argv[2] : "", part);

        double shotAt = -1.0;
        std::string shotPath;
        if (const char* shot = std::getenv("VOXEL_AUTO_SCREENSHOT")) {
            const std::string spec = shot;
            if (const size_t colon = spec.find(':'); colon != std::string::npos) {
                shotAt = std::atof(spec.substr(0, colon).c_str());
                shotPath = spec.substr(colon + 1);
            }
        }

        float clickX = -1.0f, clickY = -1.0f;
        if (const char* c = std::getenv("VOXEL_EDITOR_CLICK")) std::sscanf(c, "%f,%f", &clickX, &clickY);
        if (const char* t = std::getenv("VOXEL_EDITOR_TOOL")) editor.SetTool(std::atoi(t));
        if (const char* a = std::getenv("VOXEL_EDITOR_ANIM")) {
            // "attack" or "attack:0.15" (held at that time).
            const std::string spec = a;
            const size_t colon = spec.find(':');
            editor.ShowAnimation(spec.substr(0, colon), colon == std::string::npos ? -1.0f : static_cast<float>(std::atof(spec.c_str() + colon + 1)));
        }
        int frames = 0;
        const double start = glfwGetTime();
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            int fbw, fbh, ww, wh;
            glfwGetFramebufferSize(window, &fbw, &fbh);
            glfwGetWindowSize(window, &ww, &wh);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            glViewport(0, 0, fbw, fbh);
            glClearColor(0.12f, 0.12f, 0.14f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            const float time = static_cast<float>(glfwGetTime() - start);
            editor.Frame(fbw, fbh, ww, wh, time);
            if (++frames == 3 && clickX >= 0.0f) editor.ClickAt(clickX, clickY);
            if (frames >= 4 && frames <= 12 && std::getenv("VOXEL_EDITOR_DO")) {
                // One action every other frame (so each is a step of its own).
                std::stringstream in(std::getenv("VOXEL_EDITOR_DO"));
                std::string act;
                for (int k = 0; std::getline(in, act, ','); k++)
                    if (frames == 4 + 2 * k) editor.Do(act);
            }

            glViewport(0, 0, fbw, fbh);
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (shotAt >= 0.0 && time >= shotAt) {
                SaveScreenshot(shotPath, fbw, fbh);
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
            glfwSwapBuffers(window);
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return result;
}
