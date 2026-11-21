#include "render/PlayerModels.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "render/Shader.h"
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

constexpr float kWidth = 0.6f, kEyeHeight = 1.62f;
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
        const float eye = p.crouching ? 1.27f : kEyeHeight;
        const float bodyHeight = eye - kHead * 0.5f;
        box(glm::scale(glm::translate(base, glm::vec3(0, bodyHeight * 0.5f, 0)), glm::vec3(kWidth * 0.5f, bodyHeight, kWidth)), color);

        const glm::mat4 head = glm::rotate(glm::translate(base, glm::vec3(p.crouching ? 0.12f : 0.0f, eye, 0)), glm::radians(p.pitch), glm::vec3(0, 0, 1));
        box(glm::scale(head, glm::vec3(kHead)), color * 1.1f);
        // Visor on the front (+x in the head's frame).
        box(glm::scale(glm::translate(head, glm::vec3(kHead * 0.5f, 0.03f, 0)), glm::vec3(0.04f, kHead * 0.3f, kHead * 0.8f)), glm::vec3(0.1f));

        // Armour, a little bigger than what it covers: a cap over the top of the head, a
        // tunic over the upper body, boots at the feet.
        if (p.armour & Armour::Bit(ArmourPiece::Cap)) {
            box(glm::scale(glm::translate(head, glm::vec3(-0.01f, kHead * 0.32f, 0)), glm::vec3(kHead * 1.08f, kHead * 0.4f, kHead * 1.08f)), tint(hide));
            box(glm::scale(glm::translate(head, glm::vec3(kHead * 0.5f, kHead * 0.14f, 0)), glm::vec3(0.12f, 0.04f, kHead * 1.08f)), tint(hideDark)); // brim
        }
        if (p.armour & Armour::Bit(ArmourPiece::Tunic))
            box(glm::scale(glm::translate(base, glm::vec3(0, bodyHeight * 0.68f, 0)), glm::vec3(kWidth * 0.5f + 0.04f, bodyHeight * 0.55f, kWidth + 0.04f)),
                tint(hide));
        if (p.armour & Armour::Bit(ArmourPiece::Boots))
            box(glm::scale(glm::translate(base, glm::vec3(0.02f, 0.11f, 0)), glm::vec3(kWidth * 0.5f + 0.06f, 0.22f, kWidth + 0.04f)), tint(hideDark));

        // The right arm, held forward a little, and what's in its hand.
        const glm::mat4 shoulder = glm::rotate(glm::translate(base, glm::vec3(0.0f, bodyHeight * 0.85f, kWidth * 0.5f + 0.08f)), glm::radians(35.0f),
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
    for (const auto& p : particles) {
        shader.SetUniform("lightLevels", p.light);
        shader.SetUniform("tileRect", p.uv);
        shader.SetUniform("model", glm::scale(glm::translate(glm::mat4(1.0f), p.position), glm::vec3(p.size)));
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    shader.SetUniform("textured", 0.0f);
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
        // A box of `size` centred at `centre` in the creature's frame (+x forward).
        const glm::mat4 base = glm::rotate(glm::translate(glm::mat4(1.0f), c.feet), -glm::radians(c.yaw), glm::vec3(0, 1, 0));
        auto box = [&](const glm::mat4& frame, const glm::vec3& centre, const glm::vec3& size, const glm::vec3& color) {
            shader.SetUniform("model", glm::scale(glm::translate(frame, centre), size));
            shader.SetUniform("color", tint(color));
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        // A limb hanging from `pivot`, swung `angle` radians forward and back.
        auto limb = [&](const glm::vec3& pivot, float angle, const glm::vec3& size, const glm::vec3& color) {
            const glm::mat4 frame = glm::rotate(glm::translate(base, pivot), angle, glm::vec3(0, 0, 1));
            box(frame, glm::vec3(0.0f, -size.y * 0.5f, 0.0f), size, color);
        };
        const float stride = std::min(1.0f, c.speed / 2.0f);
        const float swing = std::sin(time * (4.0f + c.speed * 2.0f)) * 0.6f * stride;

        if (c.type == 0) { // Deer
            const glm::vec3 coat(0.58f, 0.40f, 0.24f), dark(0.36f, 0.24f, 0.14f), pale(0.88f, 0.82f, 0.70f);
            box(base, { 0.0f, 0.88f, 0.0f }, { 1.1f, 0.48f, 0.46f }, coat);           // body
            for (int i = 0; i < 4; i++) {
                const float x = i < 2 ? 0.4f : -0.4f, z = (i % 2) ? 0.15f : -0.15f;
                limb({ x, 0.66f, z }, ((i == 0 || i == 3) ? swing : -swing), { 0.12f, 0.66f, 0.12f }, dark);
            }
            const glm::mat4 neck = glm::rotate(glm::translate(base, { 0.5f, 1.02f, 0.0f }), glm::radians(-30.0f), glm::vec3(0, 0, 1));
            box(neck, { 0.0f, 0.2f, 0.0f }, { 0.2f, 0.45f, 0.2f }, coat);              // neck
            box(base, { 0.78f, 1.38f, 0.0f }, { 0.38f, 0.24f, 0.24f }, coat);          // head
            box(base, { 0.98f, 1.34f, 0.0f }, { 0.06f, 0.1f, 0.1f }, glm::vec3(0.1f)); // nose
            for (float z : { -0.08f, 0.08f }) {
                box(base, { 0.72f, 1.62f, z * 1.4f }, { 0.05f, 0.28f, 0.05f }, pale);  // antlers
                box(base, { 0.78f, 1.68f, z * 2.6f }, { 0.05f, 0.05f, 0.18f }, pale);
            }
            box(base, { -0.58f, 0.98f, 0.0f }, { 0.08f, 0.14f, 0.12f }, pale);         // tail
        }
        else if (c.type == 4) { // Bat: a little dark body, ears, and wings flapping fast
            const glm::vec3 fur(0.18f, 0.15f, 0.14f), wing(0.12f, 0.1f, 0.1f);
            box(base, { 0.0f, 0.2f, 0.0f }, { 0.18f, 0.16f, 0.14f }, fur);
            for (float z : { -0.04f, 0.04f }) box(base, { 0.06f, 0.31f, z }, { 0.03f, 0.06f, 0.03f }, fur);
            const float flap = std::sin(time * 28.0f + c.feet.x) * 0.7f;
            for (float side : { -1.0f, 1.0f }) {
                const glm::mat4 w = glm::rotate(glm::translate(base, { 0.0f, 0.22f, side * 0.07f }), side * flap, glm::vec3(1, 0, 0));
                box(w, { 0.0f, 0.0f, side * 0.16f }, { 0.16f, 0.02f, 0.32f }, wing);
            }
        }
        else if (c.type == 3) { // Wolf: grey, low and long, with a snout, ears and a bushy tail
            const glm::vec3 fur(0.55f, 0.55f, 0.56f), darkFur(0.4f, 0.4f, 0.42f), pale(0.8f, 0.8f, 0.78f);
            box(base, { 0.0f, 0.55f, 0.0f }, { 0.8f, 0.34f, 0.36f }, fur);           // body
            box(base, { 0.22f, 0.6f, 0.0f }, { 0.36f, 0.4f, 0.42f }, darkFur);       // shoulders
            for (int i = 0; i < 4; i++) {
                const float x = i < 2 ? 0.28f : -0.28f, z = (i % 2) ? 0.12f : -0.12f;
                limb({ x, 0.42f, z }, ((i == 0 || i == 3) ? swing : -swing), { 0.11f, 0.42f, 0.11f }, darkFur);
            }
            box(base, { 0.55f, 0.78f, 0.0f }, { 0.3f, 0.28f, 0.3f }, fur);          // head
            box(base, { 0.75f, 0.72f, 0.0f }, { 0.16f, 0.12f, 0.16f }, pale);       // snout
            box(base, { 0.79f, 0.76f, 0.0f }, { 0.05f, 0.04f, 0.06f }, glm::vec3(0.08f)); // nose
            for (float z : { -0.09f, 0.09f }) box(base, { 0.5f, 0.97f, z }, { 0.07f, 0.12f, 0.07f }, darkFur); // ears
            const glm::mat4 tail = glm::rotate(glm::translate(base, { -0.42f, 0.66f, 0.0f }), glm::radians(-35.0f + swing * 20.0f), glm::vec3(0, 0, 1));
            box(tail, { -0.18f, 0.0f, 0.0f }, { 0.38f, 0.12f, 0.12f }, fur);
            if (c.tame) box(base, { 0.4f, 0.74f, 0.0f }, { 0.06f, 0.3f, 0.34f }, glm::vec3(0.8f, 0.12f, 0.1f)); // collar
        }
        else if (c.type == 2) { // Skeleton: thin bone-white limbs, a skull with dark sockets, a bow
            const glm::vec3 bone(0.86f, 0.84f, 0.76f), dark(0.12f, 0.1f, 0.1f);
            for (float z : { -0.1f, 0.1f })
                limb({ 0.0f, 0.82f, z }, z < 0.0f ? swing : -swing, { 0.1f, 0.82f, 0.1f }, bone); // legs
            box(base, { 0.0f, 1.12f, 0.0f }, { 0.18f, 0.6f, 0.42f }, bone);           // ribs
            for (float y : { 0.95f, 1.1f, 1.25f }) box(base, { 0.0f, y, 0.0f }, { 0.2f, 0.03f, 0.46f }, glm::vec3(0.7f, 0.68f, 0.6f));
            box(base, { 0.02f, 1.62f, 0.0f }, { 0.38f, 0.38f, 0.38f }, bone);         // skull
            for (float z : { -0.08f, 0.08f }) box(base, { 0.2f, 1.66f, z }, { 0.02f, 0.08f, 0.08f }, dark); // sockets
            for (float z : { -0.3f, 0.3f }) {
                // Arms held out, the left with a bow.
                const glm::mat4 arm = glm::translate(base, { 0.0f, 1.36f, z });
                box(arm, { 0.28f, 0.0f, 0.0f }, { 0.56f, 0.08f, 0.08f }, bone);
            }
            box(base, { 0.58f, 1.36f, -0.3f }, { 0.05f, 0.7f, 0.05f }, glm::vec3(0.5f, 0.34f, 0.18f)); // the bow
        }
        else { // Ghoul
            const glm::vec3 skin(0.46f, 0.56f, 0.42f), rags(0.28f, 0.26f, 0.30f), eyes(1.0f, 0.85f, 0.2f);
            for (float z : { -0.12f, 0.12f })
                limb({ 0.0f, 0.82f, z }, z < 0.0f ? swing : -swing, { 0.2f, 0.82f, 0.2f }, rags); // legs
            box(base, { 0.0f, 1.12f, 0.0f }, { 0.3f, 0.62f, 0.5f }, rags);             // body
            box(base, { 0.02f, 1.62f, 0.0f }, { 0.4f, 0.4f, 0.4f }, skin);             // head
            shader.SetUniform("glow", 1.0f);
            for (float z : { -0.09f, 0.09f })
                box(base, { 0.23f, 1.66f, z }, { 0.02f, 0.06f, 0.08f }, eyes);         // eyes, glowing
            shader.SetUniform("glow", 0.0f);
            for (float z : { -0.33f, 0.33f }) {
                // Arms reaching forward, swaying.
                const glm::mat4 arm = glm::rotate(glm::translate(base, { 0.0f, 1.36f, z }), (z < 0.0f ? -swing : swing) * 0.3f, glm::vec3(0, 1, 0));
                box(arm, { 0.3f, 0.0f, 0.0f }, { 0.62f, 0.16f, 0.16f }, skin);
            }
        }
    }
}
