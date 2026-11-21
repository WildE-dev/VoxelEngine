// The model editor: a program of its own for Resources/Data/creatures.json and
// structures.json (see PlayerModels::CreatureShape for the format). It shows the model
// being edited as the game draws it, with the part picked flashing, and saves the file
// back both next to the program (so the game, after R, shows it) and in the source tree.
//
//   ModelEditor [creatures|structures] [model]
//
// Drag (left or right button) to turn round the model, shift-drag or middle-drag to
// move, scroll to zoom. Ctrl (Cmd) S saves, Z undoes, shift Z redoes; Delete removes the
// part picked.
//
// For unattended checks: VOXEL_AUTO_SCREENSHOT="seconds:path.png" saves a screenshot
// that long after starting and quits; VOXEL_EDITOR_PART="0,1" picks a part (by its place
// in the tree).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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

constexpr const char* kFiles[] = { "creatures.json", "structures.json" };
constexpr const char* kFileTitles[] = { "Creatures", "Structures" };
constexpr const char* kAxes[] = { "x", "y", "z" };
constexpr float kLeftWidth = 270.0f, kRightWidth = 360.0f;

// The order keys are written in, so the files stay easy to read.
int KeyRank(const std::string& k) {
    static const char* order[] = { "name", "inherits", "scale", "colors", "at", "rotate", "swing", "flap", "box", "size", "color", "texture", "tile", "glow", "when", "parts" };
    for (int i = 0; i < static_cast<int>(std::size(order)); i++)
        if (k == order[i]) return i;
    return 100;
}

void Tidy(Value& v) {
    if (v.IsObject()) {
        std::stable_sort(v.object.begin(), v.object.end(), [](const auto& a, const auto& b) { return KeyRank(a.first) < KeyRank(b.first); });
        for (auto& [k, child] : v.object)
            if (k != "colors") Tidy(child);
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
        if (!Json::ParseFile(path, read, error)) {
            status = error;
            read = Json::Object();
        }
        header = Json::LeadingComments(path);
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
        const std::string text = header + Json::Write(tidy);
        std::vector<std::string> wrote;
        auto put = [&](const std::string& path) {
            std::ofstream out(path, std::ios::binary);
            out << text;
            if (out) wrote.push_back(path);
            else status = "Could not write " + path;
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
        status = "Saved " + wrote.front() + (wrote.size() > 1 ? " and " + wrote.back() : "") + ". Press R in the game to see it there.";
    }

    void Frame(int fbWidth, int fbHeight, int winWidth, int winHeight, float time);

    void Pick(const std::string& m, const std::vector<int>& p) {
        if (root.Find(m)) model = m;
        path_ = p;
        if (!SelectedPart()) path_.clear();
        rebuild = true;
    }
    void OpenNamed(const std::string& name) {
        for (int i = 0; i < 2; i++)
            if (name == kFiles[i] || name + ".json" == kFiles[i]) Open(i);
    }

private:
    TextureAtlas& atlas;
    PlayerModels& models;
    int file = 0;
    Value root, committed;
    std::string header, savedText;
    std::vector<Value> undo, redo;
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
    void ResetView() {
        yaw = -50.0f, pitch = 22.0f, distance = Structures() ? 9.0f : 4.0f;
        target = glm::vec3(0.0f, Structures() ? 1.6f : 0.8f, 0.0f);
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
    void Changed() { changed = true; }

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
        if (!models.BuildShapes(root, { model }, shapes, buildError)) shapes.clear();
    }

    void Undo() {
        if (undo.empty()) return;
        redo.push_back(committed);
        root = undo.back();
        undo.pop_back();
        AfterHistory();
    }
    void Redo() {
        if (redo.empty()) return;
        undo.push_back(committed);
        root = redo.back();
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

    MenuBar();
    const float top = ImGui::GetFrameHeight();
    const float height = static_cast<float>(winHeight) - top;
    LeftPanel(top, height);
    RightPanel(static_cast<float>(winWidth) - kRightWidth, top, height);
    UnsavedPrompt();

    // The view between the panels: drag to turn, shift-drag (or middle) to move, scroll.
    const ImVec2 mouse = io.MousePos;
    const bool overView = mouse.x > kLeftWidth && mouse.x < winWidth - kRightWidth && mouse.y > top;
    if (!io.WantCaptureMouse && overView) {
        const bool move = io.MouseDown[2] || ((io.MouseDown[0] || io.MouseDown[1]) && io.KeyShift);
        if (move) {
            const glm::vec3 forward(std::cos(glm::radians(pitch)) * std::cos(glm::radians(yaw)), std::sin(glm::radians(pitch)),
                                    std::cos(glm::radians(pitch)) * std::sin(glm::radians(yaw)));
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
            const glm::vec3 up = glm::cross(right, forward);
            target += (right * io.MouseDelta.x - up * io.MouseDelta.y) * distance * 0.0025f;
        } else if (io.MouseDown[0] || io.MouseDown[1]) {
            yaw += io.MouseDelta.x * 0.4f;
            pitch = std::clamp(pitch + io.MouseDelta.y * 0.4f, -89.0f, 89.0f);
        }
        if (io.MouseWheel != 0.0f) distance = std::clamp(distance * std::pow(0.9f, io.MouseWheel), 0.5f, 40.0f);
    }
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
    if (pending && !ImGui::IsAnyItemActive()) {
        undo.push_back(committed);
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

    const glm::vec3 eye = target + distance * glm::vec3(std::cos(glm::radians(pitch)) * std::cos(glm::radians(yaw)), std::sin(glm::radians(pitch)),
                                                        std::cos(glm::radians(pitch)) * std::sin(glm::radians(yaw)));
    const glm::mat4 view = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
    const glm::mat4 projection = glm::perspective(glm::radians(50.0f), static_cast<float>(w) / static_cast<float>(h), 0.05f, 200.0f);

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
    const glm::mat4 base = Structures() ? glm::translate(glm::mat4(1.0f), glm::vec3(-1.5f, 0.0f, -1.5f)) : glm::mat4(1.0f);

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
        const float swing = walk ? std::sin(time * 6.0f) * 0.6f : 0.0f;
        models.DrawPreview(s, view, projection, it->second, base, time, swing, working, flash ? HighlightPart() : nullptr, glm::vec2(sky, block));
    }
    glDisable(GL_DEPTH_TEST);
}

void Editor::MenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        for (int i = 0; i < 2; i++)
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
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Flash the picked part", nullptr, &flash);
        ImGui::MenuItem("Ground grid", nullptr, &showGrid);
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
    ImGui::Begin("Models", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    for (int i = 0; i < 2; i++) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(kFileTitles[i], file == i) && i != file) {
            if (unsaved) askFile = i;
            else Open(i);
        }
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
    ImGui::Begin("Parts", nullptr, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    Value* m = Model();
    if (!m) {
        ImGui::TextDisabled("No model.");
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
        Changed();
    }
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
    ImGui::End();
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
