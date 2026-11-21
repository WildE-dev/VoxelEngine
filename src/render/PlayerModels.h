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
};

// A creature as drawn (world/Creatures.h): what it is, where, which way it faces,
// whether it's just been hit, and how fast it's going (for its legs).
struct CreatureModel {
    uint8_t type = 0; // CreatureType
    glm::vec3 feet{0.0f};
    float yaw = 0.0f;
    bool hurt = false;
    float speed = 0.0f;
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
    void DrawCreatures(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<CreatureModel>& creatures, float time);

    // A stable colour per player id.
    static glm::vec3 ColorFor(int id);

private:
    GLuint vao = 0, vbo = 0;
};
