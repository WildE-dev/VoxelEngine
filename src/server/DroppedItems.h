#pragma once
// Items lying in the world (the server's): what a broken block leaves, what a player
// throws away, and what won't fit in an inventory. Each is a stack of one item (and for
// a block, an offcut of units too: a broken slab is half a block of stone) with a
// little box that falls, lands on blocks and slopes, slides to a stop, floats up in
// water and gets pushed out of a block placed over it. Stacks of the same thing lying
// together join up. They're gone after kLifetime seconds.
//
// The server picks them up into players' inventories (Server::UpdateDroppedItems):
// anyone who touches one, once its delay is over. What a block left flies to the
// player who broke it (its `owner`) when they're near.
#include <cstdint>
#include <random>
#include <vector>

#include <glm/glm.hpp>

#include "world/Bodies.h"
#include "world/Collision.h"
#include "world/Inventory.h"

class BlockAccess;

class DroppedItems
{
public:
    struct Item {
        uint32_t id = 0;
        ItemId item = Items::kNone;
        uint8_t count = 0;  // whole items (0 for an offcut alone)
        uint8_t units = 0;  // and a block's offcut, less than a block
        glm::vec3 position{ 0.0f }; // the middle of its box
        glm::vec3 velocity{ 0.0f };
        float age = 0.0f;
        float pickupDelay = 0.0f; // nobody picks it up before it's this old
        uint16_t owner = 0;       // broke the block it came from (flies to them): 0 none
        float ownerFor = 0.0f;    // (until it's this old)
        bool onGround = false;
        bool flying = false;      // flying to a player: through blocks, no gravity
    };

    static float Half() { return Bodies::ItemSize() * 0.5f; } // its box: half of bodies.json's "item" size
    static constexpr float kGravity = 20.0f;
    static constexpr float kLifetime = 300.0f;  // five minutes
    static constexpr float kBlockDelay = 0.25f; // what a broken block leaves
    static constexpr float kThrowDelay = 1.5f;  // what a player threw (so it gets away from them)
    static constexpr size_t kMaxItems = 1024;   // the oldest go when there are more

    explicit DroppedItems(uint32_t seed = 0x17E35u) : rng(seed) {}

    // Adds a stack lying (or thrown) here; returns its id (0: nothing to drop).
    uint32_t Drop(ItemId item, uint32_t count, uint32_t units, const glm::vec3& position, const glm::vec3& velocity, float pickupDelay, uint16_t owner = 0);
    // What a broken block leaves: `units` of a type, as stacks (and an offcut), popping
    // out of the middle of the cell, flying to `owner` when they're near.
    void DropUnits(BlockType type, uint32_t units, const glm::ivec3& cell, uint16_t owner = 0);
    // Items (whole), popping out of a cell (a chest's contents).
    void DropItems(ItemId item, uint32_t count, const glm::ivec3& cell, uint16_t owner = 0);
    // A little upward pop in some direction, for things coming out of a block.
    glm::vec3 Pop();

    // Moves everything on by dt seconds, and joins up stacks lying together.
    void Update(const BlockAccess& world, float dt);

    // Pulls it toward `target` (flying there, through blocks).
    static void FlyTo(Item& item, const glm::vec3& target, float dt);

    std::vector<Item>& All() { return items; }
    const std::vector<Item>& All() const { return items; }
    void Remove(uint32_t id);
    void Clear() { items.clear(); }
    size_t Count() const { return items.size(); }

    static Collision::AABB Box(const glm::vec3& position) { return { position - glm::vec3(Half()), position + glm::vec3(Half()) }; }

private:
    void Move(const BlockAccess& world, Item& item, float dt);
    void Merge();
    std::vector<Item> items;
    uint32_t nextId = 1;
    float sinceMerge = 0.0f;
    std::mt19937 rng;
};
