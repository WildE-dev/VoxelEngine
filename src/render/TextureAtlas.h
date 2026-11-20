#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glad/glad.h>
#include <glm/glm.hpp>

class Shader;

// Packs every PNG in a directory into one GL texture at runtime.
//
// Tiles are addressed by file name without extension ("grass_top.png" -> "grass_top").
// Index 0 is always a generated "missing texture" tile, so a lookup never fails.
// Each tile is surrounded by a gutter of repeated edge pixels so mipmapping and
// filtering don't bleed neighbouring tiles into each other.
class TextureAtlas
{
public:
    static constexpr int kMissingIndex = 0;
    // Tile indices are packed into 10 bits per vertex (see Block.cpp).
    static constexpr int kMaxTextures = 1024;

    TextureAtlas() = default;
    ~TextureAtlas();

    TextureAtlas(const TextureAtlas&) = delete;
    TextureAtlas& operator=(const TextureAtlas&) = delete;

    // (Re)builds the atlas from the directory. Safe to call again to hot reload.
    // Returns false if no textures were found (the atlas still holds the missing tile).
    bool Build(const std::string& directory);

    // Index of the named tile, or kMissingIndex if there is no such texture.
    int GetIndex(const std::string& name) const;
    const std::vector<std::string>& GetNames() const { return names; }

    void Bind(GLenum textureUnit) const;
    // Sets the uniforms main.vert uses to turn a tile index + local uv into atlas uv.
    void ApplyUniforms(Shader& shader) const;

    GLuint GetTexture() const { return texture; }
    int GetTileSize() const { return tileSize; }
    glm::ivec2 GetSize() const { return size; }

private:
    GLuint texture = 0;
    int tileSize = 0;
    int padding = 0;
    int columns = 1;
    glm::ivec2 size{0};

    std::vector<std::string> names;
    std::unordered_map<std::string, int> indices;
};
