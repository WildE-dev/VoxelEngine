#include <algorithm>
#include "render/PlayerModels.h"

#include <cmath>
#include <functional>
#include <map>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "render/Shader.h"
#include "render/TextureAtlas.h"
#include "world/Bodies.h"
#include "world/Block.h"
#include "world/Creatures.h"
#include "world/Json.h"
#include "world/Armour.h"

namespace {

// Unit cube centred on the origin: 36 vertices of position + normal.
std::vector<float> CubeVertices() {
    const glm::vec3 normals[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    std::vector<float> v;
    for (const auto& n : normals) {
        // Two tangents spanning the face; a, b runs counter-clockwise seen from outside,
        // so the corners are taken the other way round: the renderer treats clockwise
        // as the front (glFrontFace(GL_CW), as the chunk meshes wind), and culls backs.
        const glm::vec3 a = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 b = glm::cross(n, a);
        const glm::vec3 c = n * 0.5f;
        const glm::vec3 corners[4] = { c - a * 0.5f - b * 0.5f, c + a * 0.5f - b * 0.5f, c + a * 0.5f + b * 0.5f, c - a * 0.5f + b * 0.5f };
        for (int i : { 0, 2, 1, 0, 3, 2 }) {
            v.insert(v.end(), { corners[i].x, corners[i].y, corners[i].z, n.x, n.y, n.z });
        }
    }
    return v;
}

const glm::vec3 kPartAxes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

// A part's frame in its parent's: placed, turned, posed by an animation, swung and
// flapped.
glm::mat4 FrameOf(const glm::mat4& parent, const PlayerModels::ShapePart& p, float time, float swing, float motion, float phase,
                  const PlayerModels::Animation* animation = nullptr, float animationTime = 0.0f) {
    glm::vec3 turn(0.0f), move(0.0f);
    const bool posed = animation && !p.name.empty() && PlayerModels::Sample(*animation, p.name, animationTime, turn, move);
    glm::mat4 f = glm::translate(parent, p.at + move);
    if (p.rotate.x != 0.0f) f = glm::rotate(f, glm::radians(p.rotate.x), kPartAxes[0]);
    if (p.rotate.y != 0.0f) f = glm::rotate(f, glm::radians(p.rotate.y), kPartAxes[1]);
    if (p.rotate.z != 0.0f) f = glm::rotate(f, glm::radians(p.rotate.z), kPartAxes[2]);
    if (posed) {
        if (turn.x != 0.0f) f = glm::rotate(f, glm::radians(turn.x), kPartAxes[0]);
        if (turn.y != 0.0f) f = glm::rotate(f, glm::radians(turn.y), kPartAxes[1]);
        if (turn.z != 0.0f) f = glm::rotate(f, glm::radians(turn.z), kPartAxes[2]);
    }
    if (p.swingAxis >= 0) f = glm::rotate(f, swing * p.swing, kPartAxes[p.swingAxis]);
    if (p.flapAxis >= 0) f = glm::rotate(f, std::sin(time * p.flapSpeed + phase) * p.flapAmount * motion, kPartAxes[p.flapAxis]);
    return f;
}

// (Drawn the size of the body it collides as: Resources/Data/bodies.json.)
float BodyWidth() { return Bodies::PlayerBody().width; }
constexpr float kHead = 0.5f;

} // namespace

PlayerModels::PlayerModels() {
    const auto vertices = CubeVertices();
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
}

PlayerModels::~PlayerModels() {
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
}

glm::vec3 PlayerModels::ColorFor(int id) {
    static const glm::vec3 palette[] = {
        { 0.90f, 0.35f, 0.30f }, { 0.30f, 0.55f, 0.90f }, { 0.95f, 0.75f, 0.25f }, { 0.45f, 0.80f, 0.40f },
        { 0.75f, 0.45f, 0.85f }, { 0.30f, 0.80f, 0.80f }, { 0.95f, 0.55f, 0.75f }, { 0.85f, 0.85f, 0.85f },
    };
    return palette[static_cast<unsigned>(id) % (sizeof(palette) / sizeof(palette[0]))];
}

void PlayerModels::Draw(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<PlayerModel>& players) {
    if (players.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    glBindVertexArray(vao);

    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 0.0f);
    auto box = [&](const glm::mat4& model, const glm::vec3& color) {
        shader.SetUniform("model", model);
        shader.SetUniform("color", color);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    };

    const glm::vec3 kRed(0.9f, 0.15f, 0.1f);
    const glm::vec3 hide(0.55f, 0.37f, 0.2f), hideDark(0.42f, 0.27f, 0.14f);
    for (const auto& p : players) {
        shader.SetUniform("lightLevels", p.light);
        auto tint = [&](const glm::vec3& c) { return p.hurt ? glm::mix(c, kRed, 0.55f) : c; };
        const glm::vec3 color = tint(p.color);
        // Yaw follows the camera convention: 0 degrees looks along +x, 90 along +z; +z
        // in this frame is their right.
        const glm::mat4 base = glm::rotate(glm::translate(glm::mat4(1.0f), p.feet), -glm::radians(p.yaw), glm::vec3(0, 1, 0));

        // Crouching: everything lower, the head a little forward over the knees.
        const float eye = p.crouching ? Bodies::PlayerBody().crouchEyeHeight : Bodies::PlayerBody().eyeHeight;
        const float bodyHeight = eye - kHead * 0.5f;
        box(glm::scale(glm::translate(base, glm::vec3(0, bodyHeight * 0.5f, 0)), glm::vec3(BodyWidth() * 0.5f, bodyHeight, BodyWidth())), color);

        const glm::mat4 head = glm::rotate(glm::translate(base, glm::vec3(p.crouching ? 0.12f : 0.0f, eye, 0)), glm::radians(p.pitch), glm::vec3(0, 0, 1));
        box(glm::scale(head, glm::vec3(kHead)), color * 1.1f);
        // Visor on the front (+x in the head's frame).
        box(glm::scale(glm::translate(head, glm::vec3(kHead * 0.5f, 0.03f, 0)), glm::vec3(0.04f, kHead * 0.3f, kHead * 0.8f)), glm::vec3(0.1f));

        // In a boat: a wooden hull round their legs.
        if (p.boat) {
            const glm::vec3 wood(0.55f, 0.38f, 0.2f), dark(0.4f, 0.27f, 0.14f);
            box(glm::scale(glm::translate(base, glm::vec3(0.0f, 0.12f, 0.0f)), glm::vec3(1.6f, 0.12f, 0.9f)), wood); // bottom
            for (float z : { -0.42f, 0.42f })
                box(glm::scale(glm::translate(base, glm::vec3(0.0f, 0.32f, z)), glm::vec3(1.6f, 0.3f, 0.08f)), dark); // sides
            for (float x : { -0.78f, 0.78f })
                box(glm::scale(glm::translate(base, glm::vec3(x, 0.32f, 0.0f)), glm::vec3(0.08f, 0.3f, 0.9f)), dark); // ends
        }
        // Armour, a little bigger than what it covers: a cap over the top of the head, a
        // tunic over the upper body, boots at the feet.
        if (p.armour & Armour::Bit(ArmourPiece::Cap)) {
            box(glm::scale(glm::translate(head, glm::vec3(-0.01f, kHead * 0.32f, 0)), glm::vec3(kHead * 1.08f, kHead * 0.4f, kHead * 1.08f)), tint(hide));
            box(glm::scale(glm::translate(head, glm::vec3(kHead * 0.5f, kHead * 0.14f, 0)), glm::vec3(0.12f, 0.04f, kHead * 1.08f)), tint(hideDark)); // brim
        }
        if (p.armour & Armour::Bit(ArmourPiece::Tunic))
            box(glm::scale(glm::translate(base, glm::vec3(0, bodyHeight * 0.68f, 0)), glm::vec3(BodyWidth() * 0.5f + 0.04f, bodyHeight * 0.55f, BodyWidth() + 0.04f)),
                tint(hide));
        if (p.armour & Armour::Bit(ArmourPiece::Leggings))
            box(glm::scale(glm::translate(base, glm::vec3(0.015f, bodyHeight * 0.24f, 0)), glm::vec3(BodyWidth() * 0.5f + 0.05f, bodyHeight * 0.3f, BodyWidth() + 0.04f)),
                tint(hide * 0.9f));
        if (p.armour & Armour::Bit(ArmourPiece::Mask)) {
            box(glm::scale(glm::translate(head, glm::vec3(kHead * 0.5f + 0.01f, -kHead * 0.05f, 0)), glm::vec3(0.04f, kHead * 0.5f, kHead * 0.9f)), tint(hide));
            box(glm::scale(glm::translate(head, glm::vec3(kHead * 0.5f + 0.03f, kHead * 0.08f, 0)), glm::vec3(0.03f, kHead * 0.14f, kHead * 0.8f)), tint(glm::vec3(0.65f, 0.8f, 0.9f)));
        }
        if (p.armour & Armour::Bit(ArmourPiece::Boots))
            box(glm::scale(glm::translate(base, glm::vec3(0.02f, 0.11f, 0)), glm::vec3(BodyWidth() * 0.5f + 0.06f, 0.22f, BodyWidth() + 0.04f)), tint(hideDark));
        // A steel plate over the chest, and a shield on the left side.
        if (p.armour & Armour::Bit(ArmourPiece::Plate))
            box(glm::scale(glm::translate(base, glm::vec3(0.01f, bodyHeight * 0.72f, 0)), glm::vec3(BodyWidth() * 0.5f + 0.07f, bodyHeight * 0.42f, BodyWidth() + 0.06f)),
                tint(glm::vec3(0.62f, 0.65f, 0.7f)));
        if (p.armour & Armour::Bit(ArmourPiece::OxygenTank)) {
            // Two cylinders (boxes) on the back, with a valve between.
            for (const float z : { -0.12f, 0.12f })
                box(glm::scale(glm::translate(base, glm::vec3(-(BodyWidth() * 0.25f + 0.12f), bodyHeight * 0.7f, z)), glm::vec3(0.18f, 0.5f, 0.2f)),
                    tint(glm::vec3(0.75f, 0.78f, 0.8f)));
            box(glm::scale(glm::translate(base, glm::vec3(-(BodyWidth() * 0.25f + 0.12f), bodyHeight * 0.7f + 0.3f, 0)), glm::vec3(0.08f, 0.08f, 0.3f)),
                tint(glm::vec3(0.85f, 0.65f, 0.2f)));
        }
        if (p.armour & Armour::Bit(ArmourPiece::Shield)) {
            box(glm::scale(glm::translate(base, glm::vec3(0.05f, bodyHeight * 0.6f, -(BodyWidth() * 0.5f + 0.14f))), glm::vec3(0.5f, 0.6f, 0.06f)), tint(glm::vec3(0.5f, 0.36f, 0.2f)));
            box(glm::scale(glm::translate(base, glm::vec3(0.05f, bodyHeight * 0.6f, -(BodyWidth() * 0.5f + 0.18f))), glm::vec3(0.14f, 0.14f, 0.04f)), tint(glm::vec3(0.6f, 0.62f, 0.66f)));
        }

        // The right arm, held forward a little, and what's in its hand.
        const glm::mat4 shoulder = glm::rotate(glm::translate(base, glm::vec3(0.0f, bodyHeight * 0.85f, BodyWidth() * 0.5f + 0.08f)), glm::radians(35.0f),
                                               glm::vec3(0, 0, 1));
        box(glm::scale(glm::translate(shoulder, glm::vec3(0, -0.3f, 0)), glm::vec3(0.14f, 0.6f, 0.14f)), color * 0.85f);
        if (p.held != PlayerModel::Held::Nothing) {
            const glm::mat4 hand = glm::translate(shoulder, glm::vec3(0.05f, -0.62f, 0.0f));
            shader.SetUniform("textured", 1.0f);
            shader.SetUniform("tileRect", p.heldUV);
            if (p.held == PlayerModel::Held::Block) {
                box(glm::scale(glm::translate(hand, glm::vec3(0.1f, 0.0f, 0.0f)), glm::vec3(0.22f)), tint(glm::vec3(1.0f)));
            }
            else {
                // A card facing sideways (its broad faces along the arm), pointing forward
                // and up out of the fist, like a tool held ready.
                const glm::mat4 card = glm::rotate(glm::translate(hand, glm::vec3(0.18f, 0.1f, 0.0f)), glm::radians(-35.0f), glm::vec3(0, 0, 1));
                box(glm::scale(card, glm::vec3(0.5f, 0.5f, 0.03f)), tint(glm::vec3(1.0f)));
            }
            shader.SetUniform("textured", 0.0f);
        }
    }
}

void PlayerModels::DrawArrows(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<ArrowModel>& arrows) {
    if (arrows.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 0.0f);
    glBindVertexArray(vao);
    for (const auto& a : arrows) {
        shader.SetUniform("lightLevels", a.light);
        // A frame along the arrow: x forward.
        const glm::vec3 x = glm::length(a.direction) > 1e-4f ? glm::normalize(a.direction) : glm::vec3(1, 0, 0);
        const glm::vec3 y = glm::normalize(std::abs(x.y) < 0.95f ? glm::cross(glm::cross(x, glm::vec3(0, 1, 0)), x) : glm::vec3(1, 0, 0));
        const glm::vec3 z = glm::cross(x, y);
        glm::mat4 frame(1.0f);
        frame[0] = glm::vec4(x, 0.0f);
        frame[1] = glm::vec4(y, 0.0f);
        frame[2] = glm::vec4(z, 0.0f);
        frame[3] = glm::vec4(a.position, 1.0f);
        auto part = [&](const glm::vec3& centre, const glm::vec3& size, const glm::vec3& color) {
            shader.SetUniform("model", glm::scale(glm::translate(frame, centre), size));
            shader.SetUniform("color", color);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        part({ -0.3f, 0.0f, 0.0f }, { 0.7f, 0.05f, 0.05f }, glm::vec3(0.55f, 0.38f, 0.2f));  // shaft
        part({ 0.07f, 0.0f, 0.0f }, { 0.1f, 0.09f, 0.09f }, glm::vec3(0.6f, 0.6f, 0.62f));   // head
        part({ -0.6f, 0.0f, 0.0f }, { 0.14f, 0.14f, 0.02f }, glm::vec3(0.9f, 0.9f, 0.88f)); // fletching
        part({ -0.6f, 0.0f, 0.0f }, { 0.14f, 0.02f, 0.14f }, glm::vec3(0.9f, 0.9f, 0.88f));
    }
}

void PlayerModels::DrawItems(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<ItemModel>& items, float time) {
    if (items.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 1.0f);
    shader.SetUniform("boxRepeat", glm::vec3(0.0f));
    shader.SetUniform("color", glm::vec3(1.0f));
    glBindVertexArray(vao);
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    // Where each copy of a stack sits, beside the first.
    static const glm::vec3 kOffsets[3] = { { 0.0f, 0.0f, 0.0f }, { 0.07f, 0.05f, 0.06f }, { -0.06f, 0.1f, -0.05f } };
    for (const auto& item : items) {
        if (glm::distance(eye, item.position) > 48.0f) continue;
        shader.SetUniform("lightLevels", item.light);
        shader.SetUniform("tileRect", item.uv);
        const float t = time + item.phase;
        const glm::vec3 middle = item.position + glm::vec3(0.0f, 0.06f + 0.05f * std::sin(t * 2.0f), 0.0f);
        const glm::mat4 base = glm::rotate(glm::translate(glm::mat4(1.0f), middle), t * 1.2f, glm::vec3(0, 1, 0));
        for (int c = 0; c < std::clamp(item.copies, 1, 3); c++) {
            const glm::mat4 at = glm::translate(base, kOffsets[c]);
            shader.SetUniform("model", item.cube ? glm::scale(at, glm::vec3(0.25f)) : glm::scale(at, glm::vec3(0.4f, 0.4f, 0.03f)));
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
    }
    shader.SetUniform("textured", 0.0f);
}

void PlayerModels::DrawWheels(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<WheelModel>& wheels) {
    if (wheels.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 0.0f);
    glBindVertexArray(vao);
    const glm::vec3 kWood(0.56f, 0.39f, 0.21f), kDark(0.36f, 0.24f, 0.13f), kIron(0.42f, 0.42f, 0.45f);
    for (const auto& w : wheels) {
        shader.SetUniform("lightLevels", w.light);
        // Frame: x along the axle, turned about it.
        glm::mat4 frame = glm::translate(glm::mat4(1.0f), w.centre);
        if (!w.alongX) frame = glm::rotate(frame, glm::radians(-90.0f), glm::vec3(0, 1, 0));
        const glm::mat4 spun = glm::rotate(frame, w.angle, glm::vec3(1, 0, 0));
        auto part = [&](const glm::mat4& f, const glm::vec3& centre, const glm::vec3& size, const glm::vec3& color) {
            shader.SetUniform("model", glm::scale(glm::translate(f, centre), size));
            shader.SetUniform("color", color);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        if (w.shaft) {
            part(spun, glm::vec3(0.0f), { 1.0f, 0.22f, 0.22f }, kWood);                 // the rod, end to end
            part(spun, glm::vec3(0.0f), { 0.12f, 0.3f, 0.3f }, kIron);                  // an iron band
            continue;
        }
        part(spun, glm::vec3(0.0f), { 1.0f, 0.24f, 0.24f }, kIron); // axle through the hub
        part(spun, glm::vec3(0.0f), { 0.7f, 0.5f, 0.5f }, kDark);   // hub
        // Two rims of twelve planks each, spokes out to them, and twelve paddles
        // between the rims, reaching just inside the 3 x 3 square.
        constexpr int kSpokes = 12;
        constexpr float kRim = 1.25f;
        for (int i = 0; i < kSpokes; i++) {
            const glm::mat4 r = glm::rotate(spun, i * glm::radians(360.0f / kSpokes), glm::vec3(1, 0, 0));
            for (const float x : { -0.38f, 0.38f }) {
                part(r, { x, kRim, 0.0f }, { 0.08f, 0.1f, 0.68f }, kDark);           // rim
                if (i % 2 == 0) part(r, { x, kRim * 0.5f, 0.0f }, { 0.07f, kRim, 0.08f }, kDark); // spoke
            }
            part(r, { 0.0f, kRim + 0.05f, 0.0f }, { 0.9f, 0.4f, 0.06f }, kWood); // paddle
        }
    }
}

void PlayerModels::DrawGauges(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<GaugeModel>& gauges) {
    if (gauges.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 0.0f);
    glBindVertexArray(vao);
    const glm::vec3 kRim(0.25f, 0.25f, 0.28f), kFace(0.93f, 0.9f, 0.8f), kRed(0.8f, 0.12f, 0.1f), kGreen(0.25f, 0.65f, 0.3f), kPin(0.1f, 0.1f, 0.1f);
    // Less detail further off: the marks only up close, nothing but the housing from
    // behind, and nothing at all far away (a dial's details are a lot of tiny boxes).
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    constexpr float kMarksRange = 12.0f, kDrawRange = 48.0f;
    for (const auto& g : gauges) {
        const glm::vec3 toEye = eye - g.centre;
        const float distance = glm::length(toEye);
        if (distance > kDrawRange) continue;
        const bool front = glm::dot(toEye, glm::vec3(g.facing)) > 0.0f;
        shader.SetUniform("lightLevels", g.light);
        // Local frame: +z the way it faces (level ones turned about y, upright ones
        // about x), the pipe behind it at -z.
        glm::mat4 base = glm::translate(glm::mat4(1.0f), g.centre);
        if (g.facing.y > 0) base = glm::rotate(base, glm::radians(-90.0f), glm::vec3(1, 0, 0));
        else if (g.facing.y < 0) base = glm::rotate(base, glm::radians(90.0f), glm::vec3(1, 0, 0));
        else base = glm::rotate(base, std::atan2(static_cast<float>(g.facing.x), static_cast<float>(g.facing.z)), glm::vec3(0, 1, 0));
        auto part = [&](const glm::mat4& f, const glm::vec3& centre, const glm::vec3& size, const glm::vec3& color) {
            shader.SetUniform("model", glm::scale(glm::translate(f, centre), size));
            shader.SetUniform("color", color);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        part(base, { 0.0f, 0.0f, -0.38f }, { 0.14f, 0.14f, 0.24f }, kRim);   // the stem, back to the pipe
        part(base, { 0.0f, 0.0f, -0.2f }, { 0.52f, 0.52f, 0.14f }, kRim);    // the housing
        if (!front) continue;
        const glm::mat4 frame = glm::translate(base, glm::vec3(0.0f, 0.0f, -0.14f));
        part(frame, { 0.0f, 0.0f, 0.01f }, { 0.44f, 0.44f, 0.02f }, kRim);
        part(frame, { 0.0f, 0.0f, 0.02f }, { 0.36f, 0.36f, 0.02f }, kFace);
        // Marks: the running pressure (a quarter) in green, the rest round the dial.
        for (int i = 0; i <= 8 && distance < kMarksRange; i++) {
            const float a = glm::radians(135.0f - 270.0f * i / 8.0f);
            const glm::mat4 r = glm::rotate(frame, a, glm::vec3(0, 0, 1));
            part(r, { 0.0f, 0.14f, 0.032f }, { 0.02f, 0.05f, 0.01f }, i == 2 ? kGreen : kPin);
        }
        const float angle = glm::radians(135.0f - 270.0f * std::clamp(g.pressure, 0.0f, 1.0f));
        const glm::mat4 needle = glm::rotate(frame, angle, glm::vec3(0, 0, 1));
        part(needle, { 0.0f, 0.07f, 0.04f }, { 0.025f, 0.15f, 0.01f }, kRed);
        part(frame, { 0.0f, 0.0f, 0.045f }, { 0.05f, 0.05f, 0.01f }, kPin);
    }
}

void PlayerModels::DrawDoors(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<DoorModel>& doors) {
    if (doors.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 1.0f);
    shader.SetUniform("boxRepeat", glm::vec3(0.0f));
    shader.SetUniform("color", glm::vec3(1.0f));
    glBindVertexArray(vao);
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    constexpr float t = Block::kDoorThickness;
    for (const auto& d : doors) {
        const glm::vec3 centre = d.origin + glm::vec3(0.5f);
        if (glm::distance(eye, centre) > 96.0f) continue;
        shader.SetUniform("lightLevels", d.light);
        shader.SetUniform("tileRect", d.uv);
        // Local frame: +z the way it faces. Shut, the panel lies across the back of the
        // cell; it turns a quarter turn about its hinge, in the middle of its thickness at
        // its left end, to lie along the inside of the cell's left side (turning about the
        // corner itself would swing it out into the block beside the doorway).
        glm::mat4 frame = glm::translate(glm::mat4(1.0f), centre);
        frame = glm::rotate(frame, std::atan2(static_cast<float>(d.facing.x), static_cast<float>(d.facing.z)), glm::vec3(0, 1, 0));
        // (Mirrored: the hinge at its right end, and it turns the other way.)
        const float side = d.mirrored ? -1.0f : 1.0f;
        frame = glm::translate(frame, glm::vec3(side * (-0.5f + t * 0.5f), 0.0f, -0.5f + t * 0.5f));
        frame = glm::rotate(frame, -side * glm::half_pi<float>() * std::clamp(d.open, 0.0f, 1.0f), glm::vec3(0, 1, 0));
        shader.SetUniform("model", glm::scale(glm::translate(frame, glm::vec3(side * (0.5f - t * 0.5f), 0.0f, 0.0f)), glm::vec3(1.0f, 1.0f, t)));
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    shader.SetUniform("textured", 0.0f);
}

void PlayerModels::DrawLevers(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<LeverModel>& levers) {
    if (levers.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 0.0f);
    glBindVertexArray(vao);
    const glm::vec3 eye = glm::vec3(glm::inverse(view)[3]);
    const glm::vec3 kStone(0.5f, 0.5f, 0.52f), kWood(0.55f, 0.38f, 0.2f), kKnob(0.75f, 0.12f, 0.1f);
    for (const auto& l : levers) {
        if (glm::distance(eye, l.centre) > 64.0f) continue;
        shader.SetUniform("lightLevels", l.light);
        // Local frame: +z out of the face it's on, +y along the way the handle tips (for a
        // floor lever, toward +z of the world; for a wall one, up).
        const glm::vec3 z(l.facing);
        const glm::vec3 y = l.facing.y != 0 ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
        const glm::vec3 x = glm::cross(y, z);
        glm::mat4 frame(1.0f);
        frame[0] = glm::vec4(x, 0.0f);
        frame[1] = glm::vec4(y, 0.0f);
        frame[2] = glm::vec4(z, 0.0f);
        frame[3] = glm::vec4(l.centre - z * 0.5f, 1.0f); // (on the face of the block behind)
        auto part = [&](const glm::mat4& f, const glm::vec3& centre, const glm::vec3& size, const glm::vec3& color) {
            shader.SetUniform("model", glm::scale(glm::translate(f, centre), size));
            shader.SetUniform("color", color);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        part(frame, { 0.0f, 0.0f, 0.0625f }, { 0.3f, 0.45f, 0.125f }, kStone); // the base
        // The handle, hinged at the base's middle, tipped 40 degrees off (toward -y) or on (+y).
        const float tilt = glm::radians(-40.0f + 80.0f * std::clamp(l.on, 0.0f, 1.0f));
        const glm::mat4 hinge = glm::rotate(glm::translate(frame, glm::vec3(0.0f, 0.0f, 0.1f)), tilt, glm::vec3(-1, 0, 0));
        part(hinge, { 0.0f, 0.0f, 0.24f }, { 0.07f, 0.07f, 0.48f }, kWood);
        part(hinge, { 0.0f, 0.0f, 0.5f }, { 0.13f, 0.13f, 0.13f }, kKnob);
    }
}

void PlayerModels::DrawGlows(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<GlowModel>& glows) {
    if (glows.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 1.0f);
    shader.SetUniform("textured", 0.0f);
    glBindVertexArray(vao);
    for (const auto& g : glows) {
        shader.SetUniform("color", glm::vec3(0.75f, 1.0f, 0.35f) * (0.25f + 0.75f * g.brightness));
        shader.SetUniform("model", glm::scale(glm::translate(glm::mat4(1.0f), g.position), glm::vec3(g.size)));
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    shader.SetUniform("glow", 0.0f);
}

void PlayerModels::DrawBolts(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<BoltModel>& bolts) {
    if (bolts.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 1.0f);
    shader.SetUniform("textured", 0.0f);
    glBindVertexArray(vao);
    for (const auto& bolt : bolts) {
        shader.SetUniform("color", glm::mix(glm::vec3(0.3f, 0.35f, 0.5f), glm::vec3(0.92f, 0.95f, 1.0f), bolt.brightness));
        for (size_t i = 0; i + 1 < bolt.points.size(); i++) {
            // A thin box along the segment: its y axis turned onto the segment.
            const glm::vec3 a = bolt.points[i], b = bolt.points[i + 1], d = b - a;
            const float length = glm::length(d);
            if (length < 1e-3f) continue;
            const glm::vec3 y = d / length;
            const glm::vec3 x = glm::normalize(std::abs(y.x) < 0.9f ? glm::cross(y, glm::vec3(1, 0, 0)) : glm::cross(y, glm::vec3(0, 0, 1)));
            const glm::vec3 z = glm::cross(x, y);
            glm::mat4 model(1.0f);
            model[0] = glm::vec4(x * 0.18f, 0.0f);
            model[1] = glm::vec4(d, 0.0f);
            model[2] = glm::vec4(z * 0.18f, 0.0f);
            model[3] = glm::vec4((a + b) * 0.5f, 1.0f);
            shader.SetUniform("model", model);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
    }
    shader.SetUniform("glow", 0.0f);
}

void PlayerModels::DrawParticles(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<ParticleModel>& particles) {
    if (particles.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 1.0f);
    shader.SetUniform("color", glm::vec3(1.0f));
    glBindVertexArray(vao);
    // The camera's right, up and back, for billboards.
    const glm::vec3 right(view[0][0], view[1][0], view[2][0]), up(view[0][1], view[1][1], view[2][1]), back(view[0][2], view[1][2], view[2][2]);
    for (const auto& p : particles) {
        shader.SetUniform("lightLevels", p.light);
        shader.SetUniform("tileRect", p.uv);
        // (A billboard faces the camera whatever the sun does: lit evenly.)
        shader.SetUniform("soft", p.billboard ? 1.0f : p.soft);
        shader.SetUniform("glow", p.glow);
        if (p.billboard) {
            // The unit cube flattened to a square in the camera's plane.
            glm::mat4 model(1.0f);
            model[0] = glm::vec4(right * p.size, 0.0f);
            model[1] = glm::vec4(up * p.size, 0.0f);
            model[2] = glm::vec4(back * (p.size * 0.01f), 0.0f);
            model[3] = glm::vec4(p.position, 1.0f);
            shader.SetUniform("model", model);
        }
        else shader.SetUniform("model", glm::scale(glm::translate(glm::mat4(1.0f), p.position), glm::vec3(p.size)));
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    shader.SetUniform("textured", 0.0f);
    shader.SetUniform("soft", 0.0f);
    shader.SetUniform("glow", 0.0f);
}

void PlayerModels::DrawCreatures(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<CreatureModel>& creatures,
                                 float time) {
    if (creatures.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    glBindVertexArray(vao);
    const glm::vec3 kRed(0.9f, 0.15f, 0.1f);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 0.0f);
    for (const auto& c : creatures) {
        shader.SetUniform("lightLevels", c.light);
        auto tint = [&](const glm::vec3& color) { return c.hurt ? glm::mix(color, kRed, 0.55f) : color; };
        auto box = [&](const glm::mat4& frame, const glm::vec3& centre, const glm::vec3& size, const glm::vec3& color) {
            shader.SetUniform("model", glm::scale(glm::translate(frame, centre), size));
            shader.SetUniform("color", tint(color));
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        // Its frame: at its feet, turned to face +x along its heading.
        const glm::mat4 base = glm::rotate(glm::translate(glm::mat4(1.0f), c.feet), -glm::radians(c.yaw), glm::vec3(0, 1, 0));
        const float stride = std::min(1.0f, c.speed / 2.0f);
        const float swing = std::sin(time * (4.0f + c.speed * 2.0f)) * 0.6f * stride;
        if (c.type >= shapes.size() || !haveShape[c.type]) {
            // No model for it: a plain box its size.
            const CreatureSpec& spec = SpecOf(static_cast<CreatureType>(c.type));
            box(base, { 0.0f, spec.height * 0.5f, 0.0f }, { spec.halfWidth * 2.0f, spec.height, spec.halfWidth * 2.0f }, glm::vec3(0.6f));
            continue;
        }
        // Its animation: what it's doing, while that lasts; else an idle one, if any.
        const CreatureShape& shape = shapes[c.type];
        const Animation* animation = nullptr;
        float animationTime = 0.0f;
        if (c.action != 0) {
            const auto it = shape.animations.find(c.action == 1 ? "attack" : "work");
            if (it != shape.animations.end() && (it->second.loop || c.actionTime <= it->second.length)) animation = &it->second, animationTime = c.actionTime;
        }
        if (!animation)
            if (const auto it = shape.animations.find("idle"); it != shape.animations.end()) animation = &it->second, animationTime = time + c.feet.x;
        DrawShape(shader, base, shape, time, swing, 1.0f, c.tame, false, c.hurt, c.feet.x, nullptr, animation, animationTime);
    }
}

bool PlayerModels::Sample(const Animation& a, const std::string& part, float time, glm::vec3& rotate, glm::vec3& move) {
    const auto it = a.tracks.find(part);
    if (it == a.tracks.end() || it->second.empty()) return false;
    const std::vector<Key>& keys = it->second;
    float t = time;
    if (a.loop && a.length > 0.0f) t = std::fmod(std::max(time, 0.0f), a.length);
    if (t <= keys.front().t) {
        rotate = keys.front().rotate, move = keys.front().move;
        return true;
    }
    for (size_t i = 0; i + 1 < keys.size(); i++) {
        const Key& k0 = keys[i];
        const Key& k1 = keys[i + 1];
        if (t > k1.t) continue;
        float u = k1.t > k0.t ? (t - k0.t) / (k1.t - k0.t) : 1.0f;
        u = u * u * (3.0f - 2.0f * u); // eased in and out
        rotate = glm::mix(k0.rotate, k1.rotate, u), move = glm::mix(k0.move, k1.move, u);
        return true;
    }
    rotate = keys.back().rotate, move = keys.back().move;
    return true;
}

void PlayerModels::DrawShape(Shader& shader, const glm::mat4& base, const CreatureShape& shape, float time, float swing, float motion, bool tame, bool working,
                             bool hurt, float phase, const ShapePart* highlight, const Animation* animation, float animationTime) {
    const glm::vec3 kRed(0.9f, 0.15f, 0.1f);
    std::function<void(const glm::mat4&, const ShapePart&)> draw = [&](const glm::mat4& parent, const ShapePart& p) {
        if ((p.when == "tame" && !tame) || (p.when == "working" && !working) || (p.when == "idle" && working)) return;
        const glm::mat4 f = FrameOf(parent, p, time, swing, motion, phase, animation, animationTime);
        if (p.hasBox) {
            if (p.glow) shader.SetUniform("glow", 1.0f);
            if (highlight) shader.SetUniform("highlight", &p == highlight ? 0.35f + 0.3f * std::sin(time * 8.0f) : 0.0f);
            shader.SetUniform("model", glm::scale(glm::translate(f, p.box), p.size));
            shader.SetUniform("color", hurt ? glm::mix(p.color, kRed, 0.55f) : p.color);
            if (!p.textured) glDrawArrays(GL_TRIANGLES, 0, 36);
            else {
                // The cube's faces in order: +x, -x, +y (top), -y (bottom), +z, -z.
                shader.SetUniform("textured", 1.0f);
                shader.SetUniform("boxRepeat", p.tiled ? p.size * shape.scale : glm::vec3(0.0f));
                if (p.uv[0] == p.uv[2] && p.uv[1] == p.uv[2]) {
                    shader.SetUniform("tileRect", p.uv[2]);
                    glDrawArrays(GL_TRIANGLES, 0, 36);
                }
                else {
                    shader.SetUniform("tileRect", p.uv[2]);
                    glDrawArrays(GL_TRIANGLES, 0, 12);
                    glDrawArrays(GL_TRIANGLES, 24, 12);
                    shader.SetUniform("tileRect", p.uv[0]);
                    glDrawArrays(GL_TRIANGLES, 12, 6);
                    shader.SetUniform("tileRect", p.uv[1]);
                    glDrawArrays(GL_TRIANGLES, 18, 6);
                }
                shader.SetUniform("textured", 0.0f);
                shader.SetUniform("boxRepeat", glm::vec3(0.0f));
            }
            if (p.glow) shader.SetUniform("glow", 0.0f);
        }
        for (const auto& child : p.parts) draw(f, child);
    };
    const glm::mat4 scaled = glm::scale(base, glm::vec3(shape.scale));
    for (const auto& p : shape.parts) draw(scaled, p);
}

void PlayerModels::DrawStructures(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<StructureModel>& structures, float time) {
    if (structures.empty()) return;
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 0.0f);
    glBindVertexArray(vao);
    for (const auto& s : structures) {
        auto it = structureShapes.find(s.id);
        if (it == structureShapes.end()) continue;
        shader.SetUniform("lightLevels", s.light);
        // Its frame: the pattern's corner, turned about its middle column.
        const glm::mat4 base = glm::translate(glm::rotate(glm::translate(glm::mat4(1.0f), s.origin + glm::vec3(1.5f, 0.0f, 1.5f)), glm::radians(-90.0f * s.turn), glm::vec3(0, 1, 0)),
                                              glm::vec3(-1.5f, 0.0f, -1.5f));
        DrawShape(shader, base, it->second, time, 0.0f, s.working ? 1.0f : 0.0f, false, s.working, false, 0.0f);
    }
}

namespace {

glm::vec3 Vec3(const Json::Value* v, glm::vec3 fallback = glm::vec3(0.0f)) {
    if (!v || !v->IsArray() || v->array.size() != 3) return fallback;
    return { static_cast<float>(v->array[0].number), static_cast<float>(v->array[1].number), static_cast<float>(v->array[2].number) };
}

int Axis(const std::string& s) { return s == "x" ? 0 : s == "y" ? 1 : s == "z" ? 2 : -1; }

using Palette = std::map<std::string, glm::vec3>;

bool Colour(const std::string& s, const Palette& palette, glm::vec3& out) {
    if (s.size() == 7 && s[0] == '#') {
        const unsigned long rgb = std::strtoul(s.c_str() + 1, nullptr, 16);
        out = glm::vec3((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF) / 255.0f;
        return true;
    }
    auto it = palette.find(s);
    if (it == palette.end()) return false;
    out = it->second;
    return true;
}

using Resolver = std::function<bool(const std::string&, glm::vec4&)>;

bool ReadPart(const Json::Value& v, const Palette& palette, const Resolver& texture, PlayerModels::ShapePart& p, const std::string& where, std::string& error) {
    p.at = Vec3(v.Find("at"));
    p.rotate = Vec3(v.Find("rotate"));
    if (const auto* sw = v.Find("swing"); sw && sw->IsObject()) {
        p.swingAxis = Axis(sw->StringOr("axis", "z"));
        p.swing = static_cast<float>(sw->NumberOr("amount", 1.0));
    }
    if (const auto* fl = v.Find("flap"); fl && fl->IsObject()) {
        p.flapAxis = Axis(fl->StringOr("axis", "x"));
        p.flapSpeed = static_cast<float>(fl->NumberOr("speed", 10.0));
        p.flapAmount = static_cast<float>(fl->NumberOr("amount", 0.5));
    }
    if (v.Find("size")) {
        p.hasBox = true;
        p.box = Vec3(v.Find("box"));
        p.size = Vec3(v.Find("size"));
        const std::string colour = v.StringOr("color", "#ffffff");
        if (!Colour(colour, palette, p.color)) {
            error = where + ": no colour called '" + colour + "'";
            return false;
        }
        // A texture: one name for every face, or top, bottom and side.
        if (const auto* tex = v.Find("texture")) {
            std::string names[3];
            if (tex->IsString()) names[0] = names[1] = names[2] = tex->string;
            else if (tex->IsObject()) {
                const std::string side = tex->StringOr("side", tex->StringOr("all", ""));
                names[0] = tex->StringOr("top", side), names[1] = tex->StringOr("bottom", side), names[2] = side;
            }
            for (int f = 0; f < 3; f++)
                if (!names[f].empty() && texture(names[f], p.uv[f])) p.textured = true;
            for (int f = 0; f < 3; f++)
                if (names[f].empty()) p.uv[f] = p.uv[2];
            p.tiled = v.BoolOr("tile", false);
        }
    }
    p.glow = v.BoolOr("glow", false);
    p.when = v.StringOr("when", "");
    p.name = v.StringOr("name", "");
    if (const auto* kids = v.Find("parts"); kids && kids->IsArray())
        for (const auto& k : kids->array) {
            PlayerModels::ShapePart child;
            if (!ReadPart(k, palette, texture, child, where, error)) return false;
            p.parts.push_back(std::move(child));
        }
    return true;
}

} // namespace

bool PlayerModels::LoadShapes(const std::string& path, const std::vector<std::string>& names, std::map<std::string, CreatureShape>& out, std::string& error) {
    Json::Value root;
    out.clear();
    if (!Json::ParseDataPath(path, root, error)) return false;
    if (!BuildShapes(root, names, out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool PlayerModels::BuildShapes(const Json::Value& root, const std::vector<std::string>& names, std::map<std::string, CreatureShape>& out, std::string& error) const {
    const std::string path = "models";
    out.clear();
    // Textures, from the block atlas (none before there is one).
    const Resolver texture = [&](const std::string& name, glm::vec4& uv) {
        if (!atlas) return false;
        const int index = atlas->GetIndex(name);
        if (index == TextureAtlas::kMissingIndex) return false;
        glm::vec2 a, b;
        atlas->GetTileUV(index, a, b);
        uv = glm::vec4(a, b);
        return true;
    };
    if (!root.IsObject()) {
        error = path + ": the file should be one { ... } of models";
        return false;
    }
    // A model may inherit another's colours and parts ("inherits"), then add its own
    // colours (replacing any of the same name) and parts.
    std::vector<const Json::Value*> animationLists; // (the model being gathered's, its own last)
    std::function<bool(const std::string&, Palette&, std::vector<const Json::Value*>&, float&, int)> gather =
        [&](const std::string& name, Palette& palette, std::vector<const Json::Value*>& partLists, float& scale, int depth) {
            const Json::Value* m = root.Find(name);
            if (!m || !m->IsObject() || depth > 8) {
                error = path + ": no model called '" + name + "'";
                return false;
            }
            if (const auto* parent = m->Find("inherits"); parent && parent->IsString() && !gather(parent->string, palette, partLists, scale, depth + 1)) return false;
            scale = static_cast<float>(m->NumberOr("scale", scale));
            if (const auto* colours = m->Find("colors"); colours && colours->IsObject())
                for (const auto& [k, v] : colours->object) {
                    glm::vec3 c;
                    if (v.IsString() && Colour(v.string, palette, c)) palette[k] = c;
                }
            if (const auto* parts = m->Find("parts"); parts && parts->IsArray()) partLists.push_back(parts);
            if (const auto* animations = m->Find("animations"); animations && animations->IsObject()) animationLists.push_back(animations);
            return true;
        };
    // The models asked for (all of them, if none named; one only there to inherit from
    // isn't built by itself).
    std::vector<std::string> wanted = names;
    if (wanted.empty())
        for (const auto& [id, value] : root.object) wanted.push_back(id);
    for (const auto& id : wanted) {
        if (!root.Find(id)) continue;
        Palette palette;
        std::vector<const Json::Value*> partLists;
        float scale = 1.0f;
        animationLists.clear();
        if (!gather(id, palette, partLists, scale, 0)) return false;
        CreatureShape shape;
        shape.scale = scale;
        // Its animations (one of the same name as one it inherits replacing it).
        for (const auto* list : animationLists)
            for (const auto& [name, a] : list->object) {
                Animation animation;
                animation.length = static_cast<float>(std::max(0.01, a.NumberOr("length", 1.0)));
                animation.loop = a.BoolOr("loop", false);
                if (const auto* tracks = a.Find("parts"); tracks && tracks->IsObject())
                    for (const auto& [part, keys] : tracks->object) {
                        if (!keys.IsArray()) continue;
                        std::vector<Key>& track = animation.tracks[part];
                        for (const auto& k : keys.array) {
                            Key key;
                            key.t = static_cast<float>(k.NumberOr("t", 0.0));
                            key.rotate = Vec3(k.Find("rotate"));
                            key.move = Vec3(k.Find("move"));
                            track.push_back(key);
                        }
                        std::stable_sort(track.begin(), track.end(), [](const Key& x, const Key& y) { return x.t < y.t; });
                    }
                shape.animations[name] = std::move(animation);
            }
        for (const auto* list : partLists) {
            if (list == partLists.back() && root.Find(id)->Find("parts") == list) shape.ownStart = shape.parts.size();
            for (const auto& v : list->array) {
                ShapePart p;
                if (!ReadPart(v, palette, texture, p, id, error)) return false;
                shape.parts.push_back(std::move(p));
            }
        }
        if (!root.Find(id)->Find("parts")) shape.ownStart = shape.parts.size();
        out[id] = std::move(shape);
    }
    return true;
}

bool PlayerModels::LoadCreatureModels(const std::string& path, std::string& error) {
    shapes.assign(static_cast<size_t>(CreatureType::Count), CreatureShape{});
    haveShape.assign(static_cast<size_t>(CreatureType::Count), false);
    std::map<std::string, CreatureShape> all;
    std::vector<std::string> ids;
    for (int t = 0; t < static_cast<int>(CreatureType::Count); t++) ids.push_back(SpecOf(static_cast<CreatureType>(t)).id);
    if (!LoadShapes(path, ids, all, error)) return false;
    for (int t = 0; t < static_cast<int>(CreatureType::Count); t++) {
        auto it = all.find(SpecOf(static_cast<CreatureType>(t)).id);
        if (it == all.end()) continue; // (drawn as a box)
        shapes[t] = it->second;
        haveShape[t] = true;
    }
    return true;
}

bool PlayerModels::LoadStructureModels(const std::string& path, std::string& error) { return LoadShapes(path, {}, structureShapes, error); }

void PlayerModels::Place(const CreatureShape& shape, const glm::mat4& base, float time, float swing, bool working, std::vector<PlacedPart>& out,
                         const Animation* animation, float animationTime) {
    out.clear();
    std::function<void(const glm::mat4&, const ShapePart&)> walk = [&](const glm::mat4& parent, const ShapePart& p) {
        if ((p.when == "working" && !working) || (p.when == "idle" && working)) return;
        const glm::mat4 f = FrameOf(parent, p, time, swing, working ? 1.0f : 0.0f, 0.0f, animation, animationTime);
        out.push_back({ &p, parent, f });
        for (const auto& c : p.parts) walk(f, c);
    };
    const glm::mat4 scaled = glm::scale(base, glm::vec3(shape.scale));
    for (const auto& p : shape.parts) walk(scaled, p);
}

void PlayerModels::DrawBoxes(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const std::vector<std::pair<glm::mat4, glm::vec3>>& boxes) {
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 1.0f);
    shader.SetUniform("textured", 0.0f);
    shader.SetUniform("highlight", 0.0f);
    glBindVertexArray(vao);
    for (const auto& [m, c] : boxes) {
        shader.SetUniform("model", m);
        shader.SetUniform("color", c);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    shader.SetUniform("glow", 0.0f);
}

void PlayerModels::DrawPreview(Shader& shader, const glm::mat4& view, const glm::mat4& projection, const CreatureShape& shape, const glm::mat4& base, float time,
                               float swing, bool working, const ShapePart* highlight, const glm::vec2& light, const Animation* animation, float animationTime) {
    shader.Use();
    shader.SetUniform("view", view);
    shader.SetUniform("projection", projection);
    shader.SetUniform("glow", 0.0f);
    shader.SetUniform("textured", 0.0f);
    shader.SetUniform("lightLevels", light);
    glBindVertexArray(vao);
    DrawShape(shader, base, shape, time, swing, working ? 1.0f : 0.0f, true, working, false, 0.0f, highlight, animation, animationTime);
    shader.SetUniform("highlight", 0.0f);
}
