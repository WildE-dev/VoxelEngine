// Block shapes, mountains and thin water. Checks that
//  - natural blocks slope and built blocks step: standing heights follow the quarters,
//    and the water a shape holds follows its mean height,
//  - chiselling lowers and raises a quarter, back to a full block or away to nothing,
//    and a slope with three corners gone falls away,
//  - the mesher draws a stepped block as quarter-columns (tops at each quarter's
//    height), joins natural slopes across rows (no notch where a slope climbs into the
//    row above), and lets a thin layer of water on water thin out to nothing,
//  - logs lie along the face they're placed against: their ends show end grain, the
//    bark's grain runs along them, the axis survives encoding,
//  - over the network, a chisel stroke reaches the other player, machines can't be
//    chiselled, and a log placed sideways is seen sideways,
//  - mountains: high peaks with snow, bare rock above the tree line with nothing
//    growing there, and a player spawning on a peak lands on top of it.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "app/Player.h"
#include "client/ChunkMesher.h"
#include "client/Client.h"
#include "client/ClientWorld.h"
#include "server/Server.h"
#include "world/BlockAccess.h"
#include "world/Coords.h"
#include "world/Raycast.h"
#include "world/TerrainGenerator.h"

// Test clients ask for a short view (chunks): plenty for the checks, and light enough
// to run every suite at once.
constexpr int kTestViewDistance = 4;

namespace {

int failures = 0;

void Check(bool ok, const std::string& what) {
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (!ok) failures++;
}

Block Shaped(BlockType type, int e2, int e1, int e0, int e3) {
    EdgeData d;
    d.SetTopY(2, static_cast<uint8_t>(e2));
    d.SetTopY(1, static_cast<uint8_t>(e1));
    d.SetTopY(0, static_cast<uint8_t>(e0));
    d.SetTopY(3, static_cast<uint8_t>(e3));
    return Block(type, d);
}

// Vertex positions (block units, within the chunk) from packed vertices.
std::vector<glm::vec3> Positions(const std::vector<uint32_t>& vertices, bool water) {
    std::vector<glm::vec3> out;
    for (size_t i = 0; i < vertices.size(); i += 2) {
        const uint32_t p = vertices[i];
        const float fine = water ? ((vertices[i + 1] >> 18) & 0x3u) / 32.0f : 0.0f;
        out.emplace_back(((p >> 24) & 0xFF) / 8.0f, ((p >> 16) & 0xFF) / 8.0f - 1.0f - fine, ((p >> 8) & 0xFF) / 8.0f);
    }
    return out;
}

// Meshes a single chunk of the given blocks (no neighbours).
void Mesh(const Chunk::Blocks& blocks, std::vector<uint32_t>& vertices, std::vector<uint32_t>& water) {
    Chunk chunk({ 0, 0, 0 });
    chunk.SetGenerated(blocks);
    Chunk::Light light;
    light.fill(0xF0);
    chunk.SetLightData(light);
    ChunkMesher::Build(chunk, Chunk::Neighbours{}, vertices, water);
}

// Generated chunks on demand, as a BlockAccess (for Player::Spawn).
struct Generated : BlockAccess {
    TerrainGenerator terrain;
    mutable std::unordered_map<glm::ivec3, std::unique_ptr<TerrainGenerator::Blocks>, Coords::IVec3Hash> chunks;
    bool GetBlock(int x, int y, int z, Block& block) const override {
        const glm::ivec3 c = Coords::ChunkOf({ x, y, z });
        auto& chunk = chunks[c];
        if (!chunk) {
            chunk = std::make_unique<TerrainGenerator::Blocks>();
            TerrainGenerator::ColumnHeights heights;
            terrain.ComputeColumnHeights(c.x, c.z, heights);
            terrain.GenerateChunk(c, heights, *chunk);
        }
        const glm::ivec3 l = Coords::LocalOf({ x, y, z });
        block = (*chunk)[Chunk::Index(l.x, l.y, l.z)];
        return true;
    }
};

struct TestClient {
    ClientWorld world;
    Client client{ world };
};

bool Pump(std::initializer_list<TestClient*> clients, double seconds, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto* c : clients) c->client.Poll();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

} // namespace

int main() {
    std::cout << "Shapes" << std::endl;
    {
        Check(StyleOf(BlockType::DIRT) == ShapeStyle::Sloped && StyleOf(BlockType::SNOW) == ShapeStyle::Sloped &&
                  StyleOf(BlockType::STONE_BRICK) == ShapeStyle::Stepped && StyleOf(BlockType::LOG) == ShapeStyle::Stepped,
              "ground slopes; built blocks step");
        const Block steps = Shaped(BlockType::STONE_BRICK, 8, 6, 2, 4); // quarters (0,0)=8 (1,0)=6 (0,1)=2 (1,1)=4
        const Block slope = Shaped(BlockType::DIRT, 8, 6, 2, 4);
        Check(steps.SurfaceHeight(0.2f, 0.2f) == 1.0f && steps.SurfaceHeight(0.8f, 0.2f) == 0.75f && steps.SurfaceHeight(0.2f, 0.8f) == 0.25f &&
                  steps.SurfaceHeight(0.8f, 0.8f) == 0.5f,
              "a stepped block is stood on at each quarter's height");
        Check(std::abs(slope.SurfaceHeight(0.5f, 0.5f) - 0.5f) < 0.01f && slope.SurfaceHeight(0.2f, 0.2f) < 1.0f && slope.SurfaceHeight(0.2f, 0.2f) > 0.6f,
              "a sloped one in between its corners");
        Check(steps.WaterFloor() == Block::kMaxWaterVolume * 20 / 32, "the water a stepped block holds sits at its quarters' mean height");

        Block b(BlockType::STONE_BRICK);
        b = b.Chiselled(2, -2);
        Check(b.type == BlockType::STONE_BRICK && !b.IsFullBlock() && b.edgeData.GetTopY(2) == 6, "chiselling lowers one quarter by a quarter");
        Check(b.Chiselled(2, 2).IsFullBlock(), "and raising it again makes the block full");
        Block gone = b;
        for (int e = 0; e < 4; e++)
            for (int i = 0; i < 4; i++) gone = gone.Chiselled(e, -2);
        Check(gone.IsAir(), "lowering every quarter all the way removes the block");
        Block one(BlockType::STONE_BRICK);
        for (int e : { 0, 1, 3 })
            for (int i = 0; i < 4; i++) one = one.Chiselled(e, -2);
        Check(one.type == BlockType::STONE_BRICK, "a stepped block stands on a single quarter");
        Block dirt(BlockType::DIRT);
        for (int e : { 0, 1, 3 })
            for (int i = 0; i < 4; i++) dirt = dirt.Chiselled(e, -2);
        Check(dirt.IsAir(), "a slope with three corners gone falls away");
        Check(!Block(BlockType::PUMP).CanChisel() && !Block::Water(128).CanChisel() && Block(BlockType::LOG).CanChisel(), "machines and water can't be chiselled");
    }

    std::cout << "Logs" << std::endl;
    {
        // No atlas here: give every texture name its own index, so faces can be told apart.
        std::map<std::string, int> names;
        ResolveBlockTextures([&](const std::string& name) { return names.emplace(name, static_cast<int>(names.size()) + 1).first->second; });
        Check(Block::Placed(BlockType::LOG, 8, 0).axis == Block::kAlongX && Block::Placed(BlockType::LOG, 8, 2).axis == Block::kAlongZ &&
                  Block::Placed(BlockType::LOG).axis == Block::kUpright,
              "a log lies along the axis of the face it's placed against");
        Check(Block::Placed(BlockType::LOG, 4, 0).axis == Block::kAlongX && !Block::Placed(BlockType::LOG, 4, 0).IsFullBlock(), "log slabs too");
        Check(Block::Placed(BlockType::STONE, 8, 0).axis == Block::kUpright && Block::Placed(BlockType::STONE_BRICK, 4, 2).axis == Block::kUpright,
              "other blocks don't turn");
        Check(Block::Placed(BlockType::LOG, 8, 0).Chiselled(0, -2).axis == Block::kAlongX, "chiselling keeps the axis");
        // A face's vertices: texture index, and which world axis the texture's v runs along.
        struct FaceLook { int texture = -1; int vAxis = -1; };
        auto look = [](const Block& b, Face face) {
            std::vector<uint32_t> v;
            b.AddFaceVertices(v, face, 0, 0, 0, FaceShading{});
            FaceLook out;
            out.texture = static_cast<int>(v[1] & 0x3FFu);
            for (size_t i = 0; i < v.size() && out.vAxis < 0; i += 2)
                for (size_t j = 0; j < v.size() && out.vAxis < 0; j += 2) {
                    const int ui = (v[i] >> 4) & 0xF, vi = v[i] & 0xF, uj = (v[j] >> 4) & 0xF, vj = v[j] & 0xF;
                    if (ui != uj || vi == vj) continue;
                    const int d[3] = { std::abs(int((v[i] >> 24) & 0xFF) - int((v[j] >> 24) & 0xFF)),
                                       std::abs(int((v[i] >> 16) & 0xFF) - int((v[j] >> 16) & 0xFF)),
                                       std::abs(int((v[i] >> 8) & 0xFF) - int((v[j] >> 8) & 0xFF)) };
                    out.vAxis = d[0] >= d[1] && d[0] >= d[2] ? 0 : d[1] >= d[2] ? 1 : 2;
                }
            return out;
        };
        const TextureData tex = GetTextureData(BlockType::LOG);
        const Block upright(BlockType::LOG), alongX = Block::Placed(BlockType::LOG, 8, 0), alongZ = Block::Placed(BlockType::LOG, 8, 2);
        Check(look(upright, Face::Top).texture == tex.top && look(upright, Face::Back).texture == tex.back && look(upright, Face::Back).vAxis == 1,
              "an upright log: end grain on top, bark up the sides");
        Check(look(alongX, Face::Right).texture == tex.top && look(alongX, Face::Left).texture == tex.bottom && look(alongX, Face::Top).texture == tex.front &&
                  look(alongX, Face::Back).texture == tex.front,
              "a log along x: end grain at its ends (east and west), bark on top and the sides");
        Check(look(alongX, Face::Top).vAxis == 0 && look(alongX, Face::Back).vAxis == 0 && look(alongX, Face::Bottom).vAxis == 0,
              "with the bark's grain running along x");
        Check(look(alongZ, Face::Back).texture == tex.top && look(alongZ, Face::Top).vAxis == 2 && look(alongZ, Face::Right).vAxis == 2,
              "and a log along z, along z");
        {
            // A stepped slab of a sideways log: its quarters' faces turn the same way.
            std::vector<uint32_t> v;
            Block::Placed(BlockType::LOG, 4, 0).AddSteppedFaces(v, Face::Right, 0, 0, 0, FaceShading{}, true);
            bool ends = !v.empty();
            for (size_t i = 1; i < v.size(); i += 2) ends &= static_cast<int>(v[i] & 0x3FFu) == tex.top;
            Check(ends, "a sideways log slab shows end grain at its ends too");
        }
        {
            // Carved logs: the faces cut inside the cell show the wood inside, the
            // outside keeps its bark and end grain.
            const int inside = GetCarvedTexture(BlockType::LOG);
            Check(inside > 0 && inside != tex.top && inside != tex.front && GetCarvedTexture(BlockType::STONE_BRICK) < 0,
                  "logs have an inside texture (other blocks look the same inside)");
            auto textures = [](const Block& b, Face face) {
                std::vector<uint32_t> v;
                b.AddSteppedFaces(v, face, 0, 0, 0, FaceShading{}, true);
                std::multiset<int> out;
                for (size_t i = 1; i < v.size(); i += 6 * 2) out.insert(static_cast<int>(v[i] & 0x3FFu)); // one per quad
                return out;
            };
            // Quarter 3 ((1,1), +x +z) cut down to half: its top and the steps up to
            // its neighbours are carved; the outside isn't.
            const Block cut = Block(BlockType::LOG).Chiselled(3, -4);
            const auto tops = textures(cut, Face::Top);
            Check(tops.count(inside) == 1 && tops.count(tex.top) == 3, "an upright log cut down: the cut quarter's top is inside wood, the rest end grain");
            // Facing +x: the quarter at (0,1) rises above the cut one, an inside step;
            // the outer faces at x = 1 (the cut quarter's lower one too) stay bark.
            const auto right = textures(cut, Face::Right);
            Check(right.count(inside) == 1 && right.count(tex.right) == 2, "the step up from the cut is inside wood, the outer sides bark");
            const auto left = textures(cut, Face::Left);
            Check(left.count(inside) == 0 && left.count(tex.left) == 2, "and the far side is untouched bark");
            const auto sideways = textures(Block::Placed(BlockType::LOG, 8, 0).Chiselled(3, -4), Face::Top);
            Check(sideways.count(inside) == 1 && sideways.count(tex.front) == 3, "a sideways log cut down shows inside wood on the cut too");
            Check(textures(Block::Placed(BlockType::LOG, 4, 1), Face::Top).count(inside) == 4, "a log slab's top is cut, so it's all inside wood");
            Check(textures(Block::Placed(BlockType::STONE_BRICK, 4, 1), Face::Top).count(GetTextureData(BlockType::STONE_BRICK).top) == 4,
                  "a brick slab's top is still brick");
        }
        net::ChunkBlocks blocks{}, back{};
        blocks[5] = alongX;
        blocks[77] = alongZ;
        blocks[300] = upright;
        net::ByteWriter w;
        net::EncodeBlockPlanes(w, blocks);
        net::ByteReader r(w.data.data(), w.data.size());
        Check(net::DecodeBlockPlanes(r, back) && r.AtEnd() && back[5].axis == Block::kAlongX && back[77].axis == Block::kAlongZ && back[300].axis == Block::kUpright,
              "the axis survives encoding");
    }

    std::cout << "Meshing" << std::endl;
    {
        // A stepped block: quarter tops at 1, 0.75, 0.25 and 0.5.
        Chunk::Blocks blocks{};
        blocks[Chunk::Index(4, 4, 4)] = Shaped(BlockType::STONE_BRICK, 8, 6, 2, 4);
        std::vector<uint32_t> v, w;
        Mesh(blocks, v, w);
        std::set<float> heights;
        for (const auto& p : Positions(v, false)) heights.insert(p.y - 4.0f);
        Check(heights == std::set<float>{ 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }, "a stepped block is drawn as quarter-columns at their heights");

        // A slope climbing more than its block: the column at x=4 rises from 4.5 at x=4
        // to a true 5.25 at x=5, clipped to the top of its block (5.0); the column at
        // x=5 holds the rest in row 5, from 5.25 up. Every block is drawn inside its own
        // cell: the first column's top stops at 5.0, not reaching up to meet the next.
        Chunk::Blocks slope{};
        for (int x = 0; x < 16; x++)
            for (int z = 0; z < 16; z++)
                for (int y = 0; y < 4; y++) slope[Chunk::Index(x, y, z)] = Block(BlockType::DIRT);
        for (int z = 0; z < 16; z++) {
            slope[Chunk::Index(4, 4, z)] = Shaped(BlockType::DIRT, 4, 8, 4, 8);
            slope[Chunk::Index(5, 4, z)] = Block(BlockType::DIRT);
            slope[Chunk::Index(5, 5, z)] = Shaped(BlockType::DIRT, 2, 6, 2, 6);
        }
        v.clear();
        w.clear();
        Mesh(slope, v, w);
        // The first column's top face: the triangles between x=4 and x=5 that start
        // above its low edge.
        const auto verts = Positions(v, false);
        float firstTop = 0.0f;
        for (size_t f = 0; f + 6 <= verts.size(); f += 6) {
            float lo = 99, hi = -99, ylo = 99, yhi = -99;
            for (size_t k = f; k < f + 6; k++) {
                lo = std::min(lo, verts[k].x), hi = std::max(hi, verts[k].x);
                ylo = std::min(ylo, verts[k].y), yhi = std::max(yhi, verts[k].y);
            }
            if (lo == 4.0f && hi == 5.0f && ylo >= 4.4f) firstTop = std::max(firstTop, yhi);
        }
        Check(firstTop == 5.0f, "a slope climbing past the top of its block stops at the top of its cell (top at " + std::to_string(firstTop) + ")");
        bool confined = true;
        for (size_t f = 0; f + 6 <= verts.size(); f += 6) {
            glm::vec3 lo(1e9f), hi(-1e9f);
            for (size_t k = f; k < f + 6; k++) lo = glm::min(lo, verts[k]), hi = glm::max(hi, verts[k]);
            const glm::vec3 cell = glm::floor(lo + glm::vec3(1e-4f));
            confined &= glm::all(glm::lessThanEqual(hi, cell + glm::vec3(1.0f + 1e-4f)));
        }
        Check(confined, "every face of the terrain's blocks lies inside its own block's cell");

        // A pool's top layer: a thin 3x3 patch on full water, empty all round it.
        Chunk::Blocks pool{};
        for (int x = 2; x <= 8; x++)
            for (int z = 2; z <= 8; z++) pool[Chunk::Index(x, 4, z)] = Block::Water(Block::kMaxWaterVolume);
        for (int x = 4; x <= 6; x++)
            for (int z = 4; z <= 6; z++) pool[Chunk::Index(x, 5, z)] = Block::Water(24);
        v.clear();
        w.clear();
        Mesh(pool, v, w);
        const auto wv = Positions(w, true);
        float lowest = 10.0f, highest = 0.0f;
        int walls = 0;
        for (size_t i = 0; i < wv.size(); i++) {
            const auto& p = wv[i];
            if (p.x < 4.0f || p.x > 7.0f || p.z < 4.0f || p.z > 7.0f || p.y < 5.0f) continue;
            const float ny = ((w[i * 2 + 1] >> 24) & 0xFu) / 7.5f - 1.0f;
            if (std::abs(ny) < 0.5f) walls++;
            else lowest = std::min(lowest, p.y), highest = std::max(highest, p.y);
        }
        Check(highest > 5.1f && lowest == 5.0f && walls == 0,
              "a thin layer of water on water thins out to nothing at its edges, with no walls (" + std::to_string(walls) + " wall vertices)");

        // A pool beside a carved brick still filling: its notch (the quarter toward the
        // pool) holds a little water, lower than the pool. The pool's surface stays level
        // up to the brick (not dipping toward the low water), and its side shows down to
        // that water, so there's no gap between them.
        Chunk::Blocks filling{};
        for (int x = 0; x <= 8; x++)
            for (int z = 0; z <= 8; z++) {
                filling[Chunk::Index(x, 3, z)] = Block(BlockType::STONE);
                const bool rim = x == 0 || x == 8 || z == 0 || z == 8 || x == 6;
                filling[Chunk::Index(x, 4, z)] = rim ? Block(BlockType::STONE) : Block::Water(Block::kMaxWaterVolume);
            }
        for (int z = 1; z <= 7; z++) filling[Chunk::Index(5, 4, z)] = Block(BlockType::STONE_BRICK).Chiselled(2, -8).WithWater(16);
        v.clear();
        w.clear();
        Mesh(filling, v, w);
        const auto fv = Positions(w, true);
        float edgeLow = 10.0f;
        int sideVerts = 0;
        for (size_t f = 0; f + 6 <= fv.size(); f += 6) {
            float lo = 99.0f, hi = -99.0f;
            for (size_t k = f; k < f + 6; k++) lo = std::min(lo, fv[k].x), hi = std::max(hi, fv[k].x);
            if (lo != 4.0f && !(lo == 5.0f && hi == 5.0f)) continue; // the pool cell beside the brick (x 4..5), or a face on x=5
            for (size_t i = f; i < f + 6; i++) {
                const auto& p = fv[i];
                const float nx = ((w[i * 2 + 1] >> 28) & 0xFu) / 7.5f - 1.0f, ny = ((w[i * 2 + 1] >> 24) & 0xFu) / 7.5f - 1.0f;
                if (p.z < 2.0f || p.z > 6.0f) continue;
                if (ny > 0.5f && lo == 4.0f && p.x == 5.0f) edgeLow = std::min(edgeLow, p.y); // its top along the brick
                if (nx > 0.5f && p.x == 5.0f && p.y > 4.6f) sideVerts++;                     // its side facing the brick, above the brick's water
            }
        }
        Check(edgeLow >= 4.9f, "a pool beside a carved block still filling stays level up to it (edge at " + std::to_string(edgeLow) + ")");
        Check(sideVerts > 0, "and its side shows above the carved block's lower water (no gap)");

        // The same brick still dry: the pool's side facing it shows over the notch half
        // right down to the notch's floor, flat along the bottom, as a half-width face (a
        // straight edge from the notch's corner up to the other quarter's would leave a
        // triangle open).
        for (int z = 1; z <= 7; z++) filling[Chunk::Index(5, 4, z)] = Block(BlockType::STONE_BRICK).Chiselled(2, -8);
        v.clear();
        w.clear();
        Mesh(filling, v, w);
        const auto dv = Positions(w, true);
        bool halfFace = false;
        for (size_t f = 0; f + 6 <= dv.size(); f += 6) {
            glm::vec3 lo(99.0f), hi(-99.0f);
            for (size_t k = f; k < f + 6; k++) lo = glm::min(lo, dv[k]), hi = glm::max(hi, dv[k]);
            const float nx = ((w[f * 2 + 1] >> 28) & 0xFu) / 7.5f - 1.0f;
            if (nx > 0.5f && lo.x == 5.0f && hi.x == 5.0f && lo.z == 3.0f && hi.z == 3.5f && lo.y == 4.0f) halfFace = true;
        }
        Check(halfFace, "beside a dry carved brick, the pool's side covers the notch right down, a half-width face with a flat bottom");

        // A pool only three quarters full beside the same dry brick, whose top stands
        // well above the water: the water stays level up to the brick (it doesn't climb
        // toward the brick's top), and nothing of it (no half side) pokes up above its
        // surface, to be seen from across the pool.
        for (int z = 1; z <= 7; z++) filling[Chunk::Index(5, 4, z)] = Block(BlockType::STONE_BRICK).Chiselled(2, -2); // notch floor 3/4 up
        for (int x = 1; x <= 4; x++)
            for (int z = 1; z <= 7; z++) filling[Chunk::Index(x, 4, z)] = Block::Water(Block::kMaxWaterVolume * 3 / 4);
        v.clear();
        w.clear();
        Mesh(filling, v, w);
        const auto pv = Positions(w, true);
        float poolHigh = 0.0f;
        for (const auto& p : pv)
            if (p.x >= 1.0f && p.x <= 5.0f && p.z >= 2.0f && p.z <= 6.0f) poolHigh = std::max(poolHigh, p.y);
        Check(poolHigh <= 4.76f, "a pool beside a carved block standing above it stays level, nothing poking up (highest " + std::to_string(poolHigh) + ")");

        // Water in a full notch beside full water in front of it, whose surface sags at
        // that corner toward a dry column beside it (surfaces average the columns round
        // a corner). The two stand at the same level, so they're one surface: the notch's
        // water meets the water in front along their shared edge, with no step or slit.
        Chunk::Blocks slit{};
        for (int x = 0; x <= 8; x++)
            for (int z = 0; z <= 8; z++) {
                slit[Chunk::Index(x, 3, z)] = Block(BlockType::STONE);
                slit[Chunk::Index(x, 4, z)] = Block(BlockType::STONE);
            }
        slit[Chunk::Index(4, 4, 4)] = Block();                                   // dry, beside the corner
        slit[Chunk::Index(5, 4, 4)] = Block::Water(Block::kMaxWaterVolume);      // the water in front
        {
            const Block notched = Block(BlockType::STONE_BRICK).Chiselled(2, -8); // notch at (0,0): toward -x and -z
            slit[Chunk::Index(5, 4, 5)] = notched.WithWater(notched.WaterCapacity());
        }
        v.clear();
        w.clear();
        Mesh(slit, v, w);
        const auto sv = Positions(w, true);
        std::map<float, float> frontTop, notchTop; // x -> height of the top along z = 5
        int between = 0;
        for (size_t f = 0; f + 6 <= sv.size(); f += 6) {
            glm::vec3 lo(99.0f), hi(-99.0f);
            for (size_t k = f; k < f + 6; k++) lo = glm::min(lo, sv[k]), hi = glm::max(hi, sv[k]);
            const float ny = ((w[f * 2 + 1] >> 24) & 0xFu) / 7.5f - 1.0f;
            if (lo.z == 5.0f && hi.z == 5.0f) between++;
            if (ny < 0.5f) continue;
            for (size_t k = f; k < f + 6; k++)
                if (sv[k].z == 5.0f) (lo.z == 4.0f ? frontTop : notchTop)[sv[k].x] = sv[k].y;
        }
        bool meets = !frontTop.empty() && frontTop.size() == notchTop.size();
        for (const auto& [x, y] : frontTop) meets &= notchTop.count(x) && std::abs(notchTop[x] - y) < 1e-4f;
        Check(meets && between == 0, "water in a full notch meets the water in front as one surface (no step, no slit; " + std::to_string(between) + " faces between)");

        // A lake over a waterlogged slope whose water is a little off the lake's level
        // (as levels settle): still one smooth surface, with no little step or side
        // between them.
        Chunk::Blocks lake{};
        for (int x = 0; x <= 8; x++)
            for (int z = 0; z <= 8; z++) {
                lake[Chunk::Index(x, 3, z)] = Block(BlockType::SAND);
                lake[Chunk::Index(x, 4, z)] = Block::Water(Block::kMaxWaterVolume * 7 / 8);
            }
        {
            Block slope = Block(BlockType::SAND);
            slope.edgeData.SetTopY(0, 2);
            slope.edgeData.SetTopY(1, 2);
            slope.edgeData.SetTopY(2, 0);
            slope.edgeData.SetTopY(3, 3);
            lake[Chunk::Index(4, 4, 4)] = slope.WithWater(slope.WaterCapacity() - 10);
        }
        v.clear();
        w.clear();
        Mesh(lake, v, w);
        const auto lv = Positions(w, true);
        int sides = 0;
        for (size_t i = 0; i < lv.size(); i++) {
            const float ny = ((w[i * 2 + 1] >> 24) & 0xFu) / 7.5f - 1.0f;
            if (std::abs(ny) < 0.5f && lv[i].x > 3.0f && lv[i].x < 6.0f && lv[i].z > 3.0f && lv[i].z < 6.0f) sides++;
        }
        Check(lake[Chunk::Index(4, 4, 4)].IsPartial() && sides == 0,
              "a waterlogged slope a little off the lake's level joins its surface, no little steps (" + std::to_string(sides) + " side vertices)");
    }

    std::cout << "Aiming and outlines" << std::endl;
    {
        // A tiny world: a stone wall at x=3, with a partial block in front of it at x=1.
        struct Little : BlockAccess {
            std::unordered_map<glm::ivec3, Block, Coords::IVec3Hash> blocks;
            bool GetBlock(int x, int y, int z, Block& b) const override {
                auto it = blocks.find({ x, y, z });
                b = it == blocks.end() ? Block() : it->second;
                return true;
            }
        } world;
        for (int y = -1; y <= 2; y++)
            for (int z = -1; z <= 2; z++) world.blocks[{ 3, y, z }] = Block(BlockType::STONE);
        const glm::vec3 east(1.0f, 0.0f, 0.0f);
        // Stepped: only the (0,0) quarter left, half a block high.
        world.blocks[{ 1, 0, 0 }] = Shaped(BlockType::STONE_BRICK, 4, 0, 0, 0);
        auto hit = Raycast(world, { -1.0f, 0.25f, 0.25f }, east, 10.0f);
        Check(hit && hit->block == glm::ivec3(1, 0, 0), "aiming at a stepped block's quarter hits it");
        hit = Raycast(world, { -1.0f, 0.25f, 0.75f }, east, 10.0f);
        Check(hit && hit->block == glm::ivec3(3, 0, 0), "aiming through its empty quarter hits the wall behind");
        hit = Raycast(world, { -1.0f, 0.75f, 0.25f }, east, 10.0f);
        Check(hit && hit->block == glm::ivec3(3, 0, 0), "and so does aiming over the quarter's top");
        // Sloped: rising from 0 at x=1 to half a block at x=2.
        world.blocks[{ 1, 0, 0 }] = Shaped(BlockType::DIRT, 0, 4, 0, 4);
        hit = Raycast(world, { -1.0f, 0.75f, 0.5f }, east, 10.0f);
        Check(hit && hit->block == glm::ivec3(3, 0, 0), "aiming over a slope hits what's beyond it");
        hit = Raycast(world, { -1.0f, 0.2f, 0.5f }, east, 10.0f);
        Check(hit && hit->block == glm::ivec3(1, 0, 0), "aiming into it below its surface hits the slope");
        // A thin slope (an eighth to a quarter high), aimed at from above and to the side:
        // the ray comes into its cell through the side, above the slope, and meets the
        // surface inside. The face is the top, so a block placed there goes on top (not
        // into the cell beside it).
        world.blocks[{ 1, 0, 0 }] = Shaped(BlockType::DIRT, 1, 2, 1, 2);
        const glm::vec3 from(-1.0f, 1.0f, 0.5f), to(1.5f, 0.1f, 0.5f);
        hit = Raycast(world, from, glm::normalize(to - from), 10.0f);
        Check(hit && hit->block == glm::ivec3(1, 0, 0) && hit->normal == glm::ivec3(0, 1, 0),
              "aiming down onto a thin slope over its side hits its top face (" +
                  (hit ? std::to_string(hit->normal.x) + "," + std::to_string(hit->normal.y) + "," + std::to_string(hit->normal.z) : std::string("miss")) + ")");
        world.blocks[{ 1, 0, 0 }] = Shaped(BlockType::STONE_BRICK, 2, 2, 2, 2); // a quarter-high stepped slab
        hit = Raycast(world, from, glm::normalize(to - from), 10.0f);
        Check(hit && hit->block == glm::ivec3(1, 0, 0) && hit->normal == glm::ivec3(0, 1, 0), "and onto a low stepped slab, its top too");
        world.blocks[{ 1, 0, 0 }] = Block(BlockType::STONE);
        hit = Raycast(world, from, glm::normalize(to - from), 10.0f);
        Check(hit && hit->normal == glm::ivec3(-1, 0, 0), "while a full block there is hit on its side, as before");

        Check(Block(BlockType::STONE).OutlineSegments().size() == 24, "a full block's outline is a cube (12 edges)");
        Check(Block::PressurePlate().OutlineSegments().size() == 24, "a pressure plate's is one thin box");
        Check(Shaped(BlockType::STONE_BRICK, 8, 6, 2, 4).OutlineSegments().size() == 96, "a stepped block's is its four quarters");
        Check(Shaped(BlockType::STONE_BRICK, 8, 0, 0, 0).OutlineSegments().size() == 24, "leaving out empty ones");
        const auto slope = Shaped(BlockType::DIRT, 8, 4, 0, 4).OutlineSegments(); // a corner at 0: three uprights, base, top, fold
        float top = 0.0f;
        for (const auto& p : slope) top = std::max(top, p.y);
        Check(slope.size() == 2 * (4 + 4 + 3 + 1) && top == 1.0f, "a slope's follows its corners");
    }

    std::cout << "Chisel over the network" << std::endl;
    {
        Server server;
        Server::Options options;
        options.bindHost = "127.0.0.1";
        options.port = 0;
        options.viewDistance = kTestViewDistance;
        options.log = false;
        options.worldType = TerrainGenerator::Type::Empty;
        options.defaultMode = net::GameMode::Creative; // building freely
        if (!server.Start(options)) return EXIT_FAILURE;
        server.RunInBackground();
        TestClient a, b;
        std::string error;
        a.client.Connect("127.0.0.1", server.GetPort(), "Mason", error, kTestViewDistance);
        b.client.Connect("127.0.0.1", server.GetPort(), "Watcher", error, kTestViewDistance);
        Check(Pump({ &a, &b }, 20.0, [&] {
                  Block g;
                  return a.client.IsWelcomed() && b.client.IsWelcomed() && b.world.GetBlock(9, -1, 7, g) && g.type == BlockType::GRASS;
              }),
              "two players join an empty world");
        // The spawn point is (7.5, ~7, 7.5) until a player sends a position: in reach.
        a.client.PlaceBlock({ 9, 0, 7 }, BlockType::STONE_BRICK);
        a.client.PlaceBlock({ 10, 0, 7 }, BlockType::PUMP);
        Pump({ &a, &b }, 1.0, [] { return false; });
        a.client.Chisel({ 9, 0, 7 }, 3, false);
        a.client.Chisel({ 9, 0, 7 }, 3, false);
        a.client.Chisel({ 10, 0, 7 }, 3, false);
        Block seen;
        Check(Pump({ &a, &b }, 20.0, [&] { return b.world.GetBlock(9, 0, 7, seen) && seen.edgeData.GetTopY(3) == 4; }) && seen.edgeData.GetTopY(2) == 8,
              "two chisel strokes take a quarter down by half a block, and the other player sees it");
        Check(b.world.GetBlock(10, 0, 7, seen) && seen.type == BlockType::PUMP && seen.IsFullBlock(), "a pump can't be chiselled");
        a.client.PlaceBlock({ 11, 0, 7 }, BlockType::LOG, 8, 0);
        Check(Pump({ &a, &b }, 20.0, [&] { return b.world.GetBlock(11, 0, 7, seen) && seen.type == BlockType::LOG; }) && seen.axis == Block::kAlongX,
              "a log placed against a side face is seen lying along x by the other player");
        a.client.Disconnect();
        b.client.Disconnect();
        server.Stop();
    }

    std::cout << "Mountains" << std::endl;
    {
        Generated world;
        const TerrainGenerator& t = world.terrain;
        int best = -100, px = 0, pz = 0;
        for (int x = -1500; x <= 1500; x += 8)
            for (int z = -1500; z <= 1500; z += 8) {
                const int h = t.ColumnAt(x, z).surfaceRow;
                if (h > best) best = h, px = x, pz = z;
            }
        Check(best > 80, "a peak rises to " + std::to_string(best) + " within 1500 blocks of spawn");
        Check(t.ColumnAt(px, pz).TopType(t.BiomeAt(px, pz)) == BlockType::SNOW, "with snow on top");

        // Around the peak: rock (or snow) above the tree line, nothing growing there,
        // and slopes that mostly fit one block per column.
        int high = 0, bare = 0, steep = 0, slopes = 0;
        bool nothingGrows = true;
        for (int x = px - 120; x <= px + 120; x += 2)
            for (int z = pz - 120; z <= pz + 120; z += 2) {
                const auto c = t.ColumnAt(x, z);
                const float spread = std::max({ c.corners[0], c.corners[1], c.corners[2], c.corners[3] }) - std::min({ c.corners[0], c.corners[1], c.corners[2], c.corners[3] });
                if (c.surfaceRow > 30) {
                    slopes++;
                    steep += spread > 1.0f;
                }
                if (c.surfaceRow < TerrainGenerator::kRockLine) continue;
                high++;
                const BlockType top = c.TopType(t.BiomeAt(x, z, c.surfaceRow));
                bare += top == BlockType::STONE || top == BlockType::SNOW;
                Block b;
                for (int y = c.surfaceRow; y <= c.surfaceRow + 3; y++) {
                    world.GetBlock(x, y, z, b);
                    if (b.type == BlockType::LOG || (b.IsPlant() && b.type != BlockType::PEBBLES)) nothingGrows = false;
                }
            }
        Check(high > 0 && bare == high, "every column above the tree line is bare rock or snow (" + std::to_string(high) + " checked)");
        Check(nothingGrows, "and no trees or plants grow up there (loose pebbles may lie about)");
        Check(steep * 50 < slopes, std::to_string(100 * steep / std::max(1, slopes)) + "% of mountain columns are steeper than a block per block");

        Player player(glm::vec3(px + 0.5f, 0.0f, pz + 0.5f));
        Check(player.Spawn(world) && player.GetFeetPosition().y > best && player.GetFeetPosition().y < best + 2.5f,
              "a player spawning on the peak lands on top of it (feet at " + std::to_string(player.GetFeetPosition().y) + ")");
    }

    std::cout << (failures ? "SHAPE TEST FAILED (" : "ALL SHAPE CHECKS PASSED (") << failures << " failures)" << std::endl;
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
