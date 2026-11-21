#pragma once

#include <cstdint>
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

// A pressure gauge on one side of a compressor or air tank: a round dial with a needle
// that swings from empty (pointing down and left) to full (down and right).
struct GaugeModel {
    glm::vec3 centre{0.0f};  // the block's centre
    int side = 0;            // which side: 0 +x, 1 -x, 2 +z, 3 -z
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
    // Creatures, from boxes too: a deer, a ghoul. `time` swings their legs.
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

private:
    GLuint vao = 0, vbo = 0;
};
