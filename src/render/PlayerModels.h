#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <glad/glad.h>
#include <glm/glm.hpp>

class Shader;

// One other player as drawn: feet position and look angles (degrees).
struct PlayerModel {
    glm::vec3 feet{0.0f};
    float yaw = 0.0f, pitch = 0.0f;
    glm::vec3 color{1.0f};
    bool hurt = false;
    glm::vec2 light{ 15.0f, 0.0f }; // sky and block light where they stand (0..15)
    // What's in their right hand: nothing, a block (a little textured cube) or a tool
    // or item (its icon, as a flat card). `heldUV`: the atlas tile (top-left, bottom-right).
    enum class Held : uint8_t { Nothing, Block, Icon } held = Held::Nothing;
    glm::vec4 heldUV{ 0.0f };
    uint8_t armour = 0; // armour worn (Armour::Bit each): drawn over them
    bool crouching = false; // ducked: lower, leaning forward
    bool boat = false;      // sitting in a boat
};

// A creature as drawn (world/Creatures.h): what it is, where, which way it faces,
// whether it's just been hit, and how fast it's going (for its legs).
struct CreatureModel {
    uint8_t type = 0; // CreatureType
    glm::vec3 feet{0.0f};
    float yaw = 0.0f;
    bool hurt = false;
    float speed = 0.0f;
    glm::vec2 light{ 15.0f, 0.0f }; // sky and block light where it stands (0..15)
    bool tame = false;               // a wolf with an owner: a red collar
};

// A formed multiblock structure (world/Multiblocks.h), drawn from its model.
struct StructureModel {
    std::string id;               // its model's name ("refinery")
    glm::vec3 origin{ 0.0f };     // its pattern's corner
    int turn = 0;                 // quarter turns
    bool working = false;
    glm::vec2 light{ 15.0f, 0.0f };
};

// A flying bit of a broken block: a tiny cube showing a piece of the block's texture.
struct ParticleModel {
    glm::vec3 position{0.0f};
    float size = 0.1f;
    glm::vec4 uv{ 0.0f }; // the atlas tile region it shows (top-left, bottom-right)
    glm::vec2 light{ 15.0f, 0.0f };
};

// An arrow: where it is and which way it points.
struct ArrowModel {
    glm::vec3 position{0.0f}, direction{ 1.0f, 0.0f, 0.0f };
    glm::vec2 light{ 15.0f, 0.0f };
};

// A firefly: a tiny speck that lights itself (0..1 how brightly, as it blinks).
struct GlowModel {
    glm::vec3 position{0.0f};
    float size = 0.05f;
    float brightness = 1.0f;
};

// A lightning bolt: a jagged line of points from the sky down to where it struck,
// drawn glowing, fading with `brightness` (0..1).
// A water wheel (world/Power.h), 3 x 3 blocks round its hub: an axle along x or z,
// with spokes, rims and paddles turned `angle` radians round it. Or a shaft: a square
// rod along its axle, turned the same way.
struct WheelModel {
    glm::vec3 centre{0.0f};
    bool alongX = true; // the axle's direction
    float angle = 0.0f;
    glm::vec2 light{ 15.0f, 0.0f };
    bool shaft = false;
};

// A pressure gauge block fixed to a pipe: a stem back to the pipe, and a round dial in
// a housing facing away from it, with a needle that swings from empty (pointing down
// and left) to full (down and right).
struct GaugeModel {
    glm::vec3 centre{0.0f};       // the block's centre
    glm::ivec3 facing{ 0, 0, 1 }; // the way the dial faces (the pipe is behind it)
    float pressure = 0.0f;   // 0..1
    glm::vec2 light{ 15.0f, 0.0f };
};

struct BoltModel {
    std::vector<glm::vec3> points;
    float brightness = 1.0f;
};

// Draws other players as simple box figures: a body, and a head that turns and tilts
// with their view, with a dark visor on the side they're facing.
// Must be created and destroyed while the GL context is current.
class PlayerModels
{
public:
    PlayerModels();
    ~PlayerModels();
    PlayerModels(const PlayerModels&) = delete;
    PlayerModels& operator=(const PlayerModels&) = delete;

    void Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<PlayerModel>& players);
    // Creatures, from boxes too: deer, wolves, bats and dwarves. `time` swings their legs.
    void DrawArrows(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<ArrowModel>& arrows);
    void DrawGlows(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<GlowModel>& glows);
    void DrawWheels(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<WheelModel>& wheels);
    void DrawGauges(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<GaugeModel>& gauges);
    void DrawBolts(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<BoltModel>& bolts);
    // Bits of broken blocks (tiny textured cubes).
    void DrawParticles(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<ParticleModel>& particles);
    void DrawCreatures(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<CreatureModel>& creatures, float time);

    // A stable colour per player id.
    static glm::vec3 ColorFor(int id);

    // Formed multiblock structures' models from Resources/Data/structures.json (same
    // format; parts "when": "working" are drawn only while it works, and its motions
    // only move then).
    bool LoadStructureModels(const std::string& path, std::string& error);
    static constexpr const char* kStructureModelsPath = "Resources/Data/structures.json";
    // Creatures' models from Resources/Data/creatures.json (see CreatureShape); false
    // (with why) if it couldn't be read, when they're drawn as plain boxes.
    bool LoadCreatureModels(const std::string& path, std::string& error);
    static constexpr const char* kCreatureModelsPath = "Resources/Data/creatures.json";

    // A creature's model: boxes, each in a frame that may be placed, turned, swung as it
    // walks or flapped, with frames inside frames.
    struct ShapePart {
        glm::vec3 at{ 0.0f };         // the frame's origin (a limb's pivot)
        glm::vec3 rotate{ 0.0f };     // turned (degrees, about x, then y, then z)
        int swingAxis = -1;           // swung as it walks about this axis (0 x, 1 y, 2 z)...
        float swing = 0.0f;           // ...by this much of the stride
        int flapAxis = -1;            // flapped about this axis, all the time...
        float flapSpeed = 0.0f, flapAmount = 0.0f;
        bool hasBox = false;
        glm::vec3 box{ 0.0f }, size{ 0.0f }; // a box in the frame: its centre and size
        glm::vec3 color{ 1.0f };
        bool glow = false;
        std::string when;             // drawn only then ("tame")
        std::vector<ShapePart> parts; // inside this frame
    };
    struct CreatureShape {
        float scale = 1.0f;
        std::vector<ShapePart> parts;
    };

    void DrawStructures(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<struct StructureModel>& structures, float time);

private:
    GLuint vao = 0, vbo = 0;
    // Every model in a file, by name (inheritance worked out).
    bool LoadShapes(const std::string& path, const std::vector<std::string>& names, std::map<std::string, CreatureShape>& out, std::string& error);
    // One model drawn in `base` (motion: how much its flaps move, 0..1; phase: offsets them).
    void DrawShape(Shader& shader, const glm::mat4& base, const CreatureShape& shape, float time, float swing, float motion, bool tame, bool working, bool hurt,
                   float phase);
    std::map<std::string, CreatureShape> structureShapes;
    std::vector<CreatureShape> shapes; // by CreatureType (empty: not loaded)
    std::vector<bool> haveShape;
};
