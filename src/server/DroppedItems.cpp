#include "server/DroppedItems.h"

#include <algorithm>
#include <cmath>

#include "world/BlockAccess.h"

uint32_t DroppedItems::Drop(ItemId item, uint32_t count, uint32_t units, const glm::vec3& position, const glm::vec3& velocity, float pickupDelay, uint16_t owner) {
    if (!Items::Valid(item)) return 0;
    if (!Items::IsStackable(item)) units = 0;
    count = std::min<uint32_t>(count, 255);
    units = std::min<uint32_t>(units, Items::kUnits - 1);
    if (count == 0 && units == 0) return 0;
    if (items.size() >= kMaxItems) items.erase(items.begin()); // (the oldest go first)
    Item it;
    it.id = nextId++;
    if (nextId == 0) nextId = 1;
    it.item = item;
    it.count = static_cast<uint8_t>(count);
    it.units = static_cast<uint8_t>(units);
    it.position = position;
    it.velocity = velocity;
    it.pickupDelay = pickupDelay;
    it.owner = owner;
    it.ownerFor = owner ? 10.0f : 0.0f;
    items.push_back(it);
    return it.id;
}

glm::vec3 DroppedItems::Pop() {
    std::uniform_real_distribution<float> angle(0.0f, 6.2831853f), speed(0.5f, 1.5f), up(3.0f, 4.5f);
    const float a = angle(rng), s = speed(rng);
    return { std::cos(a) * s, up(rng), std::sin(a) * s };
}

void DroppedItems::DropUnits(ItemRef item, uint32_t units, const glm::ivec3& cell, uint16_t owner) {
    // (Fire, lava and the like are never anything to pick up: knocked off what held
    // them, or broken, they're just gone.)
    if (Items::IsBlock(item) && IsCreativeOnlyType(Items::BlockOf(item))) return;
    if (!Items::IsStackable(item) || units == 0) return;
    const glm::vec3 middle = glm::vec3(cell) + glm::vec3(0.5f);
    uint32_t whole = units / Items::kUnits;
    const uint32_t offcut = units % Items::kUnits;
    // Whole stacks first; the offcut rides with the last one (or alone).
    do {
        const uint32_t n = std::min(whole, Items::kMaxStack);
        whole -= n;
        Drop(item, n, whole == 0 ? offcut : 0, middle, Pop(), kBlockDelay, owner);
    } while (whole > 0);
}

void DroppedItems::DropItems(ItemId item, uint32_t count, const glm::ivec3& cell, uint16_t owner) {
    const glm::vec3 middle = glm::vec3(cell) + glm::vec3(0.5f);
    const uint32_t max = Items::MaxStack(item);
    while (count > 0) {
        const uint32_t n = std::min(count, max);
        count -= n;
        Drop(item, n, 0, middle, Pop(), kBlockDelay, owner);
    }
}

void DroppedItems::FlyTo(Item& item, const glm::vec3& target, float dt) {
    const glm::vec3 to = target - item.position;
    const float d = glm::length(to);
    const float speed = std::min(12.0f, 4.0f + d * 4.0f);
    item.velocity = d > 1e-4f ? to / d * std::min(speed, d / std::max(dt, 1e-4f)) : glm::vec3(0.0f);
    item.flying = true;
    item.onGround = false;
}

void DroppedItems::Remove(uint32_t id) {
    items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& i) { return i.id == id; }), items.end());
}

void DroppedItems::Update(const BlockAccess& world, float dt) {
    for (auto it = items.begin(); it != items.end();) {
        it->age += dt;
        if (it->age > kLifetime || it->position.y < -512.0f) {
            it = items.erase(it);
            continue;
        }
        if (it->flying) {
            it->position += it->velocity * dt;
            it->flying = false; // (the server pulls it again next tick if it still should)
        }
        else Move(world, *it, dt);
        ++it;
    }
    sinceMerge += dt;
    if (sinceMerge >= 0.5f) {
        sinceMerge = 0.0f;
        Merge();
    }
}

void DroppedItems::Move(const BlockAccess& world, Item& item, float dt) {
    glm::vec3& p = item.position;
    glm::vec3& v = item.velocity;
    const glm::ivec3 cell(glm::floor(p));
    Block here;
    if (!world.GetBlock(cell.x, cell.y, cell.z, here)) return; // (not loaded: it waits)
    // Inside a block (one was placed over it): it rises out.
    if (Collision::BoxBlocked(world, Box(p))) {
        p.y += 3.0f * dt;
        v = glm::vec3(0.0f);
        item.onGround = false;
        return;
    }
    // Water holds it up (it bobs up to the top); otherwise it falls.
    if (world.IsUnderwater(p)) {
        v.y += (1.0f - v.y) * std::min(1.0f, 4.0f * dt);
        v.x *= std::exp(-2.0f * dt), v.z *= std::exp(-2.0f * dt);
    }
    else v.y = std::max(v.y - kGravity * dt, -40.0f);
    // Lying on the ground it slides to a stop, slowly on ice.
    if (item.onGround) {
        Block under;
        const glm::ivec3 below(glm::floor(p - glm::vec3(0.0f, kHalf + 0.05f, 0.0f)));
        const bool ice = world.GetBlock(below.x, below.y, below.z, under) && under.type == BlockType::ICE;
        const float k = std::exp(-(ice ? 0.6f : 10.0f) * dt);
        v.x *= k, v.z *= k;
        if (std::abs(v.x) < 0.01f) v.x = 0.0f;
        if (std::abs(v.z) < 0.01f) v.z = 0.0f;
    }
    const int steps = std::max(1, static_cast<int>(std::ceil(glm::length(v) * dt / 0.2f)));
    const float h = dt / steps;
    for (int s = 0; s < steps; s++) {
        for (const int a : { 0, 2 }) {
            glm::vec3 next = p;
            next[a] += v[a] * h;
            if (Collision::BoxBlocked(world, Box(next))) v[a] *= -0.2f; // (a little bounce off walls)
            else p = next;
        }
        const float dy = v.y * h;
        if (dy > 0.0f) {
            glm::vec3 next = p;
            next.y += dy;
            if (Collision::BoxBlocked(world, Box(next))) v.y = 0.0f;
            else p = next;
            item.onGround = false;
            continue;
        }
        // Down (or resting): onto the highest surface under it, slopes included (from a
        // little above its bottom, so it rides up a slope it slides into).
        bool unknown = false;
        const auto ground = Collision::GroundHeight(world, Box(p), 0.3f, -dy + 0.02f, unknown);
        const float bottom = p.y - kHalf;
        if (ground && *ground >= bottom + dy - 1e-4f) {
            p.y = *ground + kHalf;
            if (!item.onGround && v.y < -4.0f) v.y = -v.y * 0.15f; // (a little bounce when it lands hard)
            else v.y = 0.0f, item.onGround = true;
        }
        else {
            p.y += dy;
            item.onGround = false;
        }
    }
}

void DroppedItems::Merge() {
    for (size_t i = 0; i < items.size(); i++) {
        Item& a = items[i];
        if (!a.onGround || a.flying) continue;
        const uint32_t max = Items::MaxStack(a.item);
        for (size_t j = i + 1; j < items.size();) {
            Item& b = items[j];
            uint32_t count = a.count + b.count;
            uint32_t units = a.units + b.units;
            if (units >= Items::kUnits) count++, units -= Items::kUnits;
            if (!b.onGround || b.flying || b.item != a.item || count > max || glm::distance(a.position, b.position) > 0.75f) {
                j++;
                continue;
            }
            a.count = static_cast<uint8_t>(count);
            a.units = static_cast<uint8_t>(units);
            // The fresher one's time left, and the longer delay still to run.
            const float waitA = std::max(0.0f, a.pickupDelay - a.age), waitB = std::max(0.0f, b.pickupDelay - b.age);
            a.age = std::min(a.age, b.age);
            a.pickupDelay = a.age + std::max(waitA, waitB);
            if (a.owner != b.owner) a.owner = 0;
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(j));
        }
    }
}
