#include "world/Bodies.h"

#include <array>
#include <iostream>

#include "world/Creatures.h"
#include "world/Json.h"

namespace Bodies {

namespace {

constexpr size_t kCreatureCount = static_cast<size_t>(CreatureType::Count);

Player player;
float itemSize = 0.25f;

std::array<CreatureSpec, kCreatureCount> Defaults() {
    std::array<CreatureSpec, kCreatureCount> specs{};
    for (size_t t = 0; t < kCreatureCount; t++) specs[t] = DefaultSpecOf(static_cast<CreatureType>(t));
    return specs;
}

std::array<CreatureSpec, kCreatureCount>& Specs() {
    static std::array<CreatureSpec, kCreatureCount> specs = Defaults();
    return specs;
}

// A size from the file: a number above zero (else, with a note, the one it had).
void Size(const Json::Value& v, const char* key, float& out, const std::string& where) {
    const Json::Value* n = v.Find(key);
    if (!n) return;
    if (!n->IsNumber() || n->number <= 0.0 || n->number > 16.0) {
        std::cerr << "[data] " << where << ": \"" << key << "\" should be a size above 0 (left as it was)" << std::endl;
        return;
    }
    out = static_cast<float>(n->number);
}

} // namespace

bool Load(const std::string& path, std::string& error) {
    Json::Value root;
    if (!Json::ParseFile(path, root, error)) return false;
    if (!root.IsObject()) {
        error = path + ": the file should be one { ... }";
        return false;
    }
    Player p;
    if (const auto* v = root.Find("player"); v && v->IsObject()) {
        const std::string where = path + ": player";
        Size(*v, "width", p.width, where);
        Size(*v, "height", p.height, where);
        Size(*v, "eyeHeight", p.eyeHeight, where);
        Size(*v, "crouchHeight", p.crouchHeight, where);
        Size(*v, "crouchEyeHeight", p.crouchEyeHeight, where);
        Size(*v, "stepHeight", p.stepHeight, where);
    }
    float item = 0.25f;
    if (const auto* v = root.Find("item"); v && v->IsObject()) Size(*v, "size", item, path + ": item");
    auto specs = Defaults();
    if (const auto* v = root.Find("creatures"); v && v->IsObject())
        for (const auto& [id, c] : v->object) {
            const CreatureType t = CreatureFromId(id);
            if (t == CreatureType::Count || !c.IsObject()) {
                std::cerr << "[data] " << path << ": no creature called '" << id << "' (left out)" << std::endl;
                continue;
            }
            CreatureSpec& s = specs[static_cast<size_t>(t)];
            float width = s.halfWidth * 2.0f;
            Size(c, "width", width, path + ": " + id);
            Size(c, "height", s.height, path + ": " + id);
            s.halfWidth = width * 0.5f;
        }
    player = p;
    itemSize = item;
    Specs() = specs;
    return true;
}

const Player& PlayerBody() { return player; }

Collision::AABB PlayerBox(const glm::vec3& feet, bool crouching) {
    const float half = player.width * 0.5f;
    return { feet - glm::vec3(half, 0.0f, half), feet + glm::vec3(half, crouching ? player.crouchHeight : player.height, half) };
}
float ItemSize() { return itemSize; }

} // namespace Bodies

const CreatureSpec& SpecOf(CreatureType type) {
    const size_t t = static_cast<size_t>(type);
    return Bodies::Specs()[t < Bodies::kCreatureCount ? t : 0];
}
