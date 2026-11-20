#include "render/TextureAtlas.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>

#include <stb_image.h>

#include "render/Shader.h"

namespace fs = std::filesystem;

namespace {

constexpr int kDefaultTileSize = 16;

struct Image {
    std::string name;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

Image MakeMissingImage(int size) {
    Image image{ "missing", size, size, std::vector<uint8_t>(size * size * 4) };
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            bool magenta = ((x * 2 / size) + (y * 2 / size)) % 2 == 0;
            uint8_t* p = &image.rgba[(y * size + x) * 4];
            p[0] = magenta ? 255 : 0;
            p[1] = 0;
            p[2] = magenta ? 255 : 0;
            p[3] = 255;
        }
    }
    return image;
}

// Nearest-neighbour resample, used when a texture doesn't match the atlas tile size.
std::vector<uint8_t> Resample(const Image& image, int size) {
    std::vector<uint8_t> out(size * size * 4);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int sx = x * image.width / size;
            int sy = y * image.height / size;
            std::copy_n(&image.rgba[(sy * image.width + sx) * 4], 4, &out[(y * size + x) * 4]);
        }
    }
    return out;
}

} // namespace

TextureAtlas::~TextureAtlas() {
    if (texture) glDeleteTextures(1, &texture);
}

bool TextureAtlas::Build(const std::string& directory) {
    // --- Load every PNG, sorted so indices are stable between runs ---
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".png") files.push_back(entry.path());
    }
    if (ec) std::cerr << "ERROR: Could not read texture directory " << directory << ": " << ec.message() << std::endl;
    std::sort(files.begin(), files.end());

    std::vector<Image> images;
    for (const auto& file : files) {
        Image image;
        image.name = file.stem().string();
        if (image.name == "missing") {
            std::cerr << "Skipping " << file << ": 'missing' is reserved" << std::endl;
            continue;
        }
        int channels;
        unsigned char* data = stbi_load(file.string().c_str(), &image.width, &image.height, &channels, 4);
        if (!data) {
            std::cerr << "ERROR: Failed to load texture " << file << ": " << stbi_failure_reason() << std::endl;
            continue;
        }
        image.rgba.assign(data, data + image.width * image.height * 4);
        stbi_image_free(data);
        images.push_back(std::move(image));
    }

    if (static_cast<int>(images.size()) > kMaxTextures - 1) {
        std::cerr << "Too many block textures (" << images.size() << "), only the first "
                  << kMaxTextures - 1 << " are used" << std::endl;
        images.resize(kMaxTextures - 1);
    }

    // The first texture decides the tile size; anything else is resampled to match.
    tileSize = images.empty() ? kDefaultTileSize : images.front().width;
    images.insert(images.begin(), MakeMissingImage(tileSize));
    for (auto& image : images) {
        if (image.width != tileSize || image.height != tileSize) {
            std::cerr << "Texture '" << image.name << "' is " << image.width << "x" << image.height
                      << ", resampling to " << tileSize << "x" << tileSize << std::endl;
            image.rgba = Resample(image, tileSize);
            image.width = image.height = tileSize;
        }
    }

    // --- Pack into a grid of padded cells ---
    padding = std::max(1, tileSize / 4);
    const int cell = tileSize + 2 * padding;
    const int count = static_cast<int>(images.size());
    columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + columns - 1) / columns;
    size = { columns * cell, rows * cell };

    std::vector<uint8_t> pixels(static_cast<size_t>(size.x) * size.y * 4, 0);
    for (int i = 0; i < count; i++) {
        const Image& image = images[i];
        const int cellX = (i % columns) * cell;
        const int cellY = (i / columns) * cell;
        // Fill the whole cell, clamping to the tile's edge pixels for the gutter.
        for (int y = 0; y < cell; y++) {
            int sy = std::clamp(y - padding, 0, tileSize - 1);
            for (int x = 0; x < cell; x++) {
                int sx = std::clamp(x - padding, 0, tileSize - 1);
                std::copy_n(&image.rgba[(sy * tileSize + sx) * 4], 4,
                            &pixels[((cellY + y) * static_cast<size_t>(size.x) + cellX + x) * 4]);
            }
        }
    }

    // Only generate mip levels that keep at least one gutter pixel and stay
    // aligned to cell boundaries; deeper levels would mix neighbouring tiles.
    int maxLevel = 0;
    while ((padding >> (maxLevel + 1)) >= 1 && cell % (1 << (maxLevel + 1)) == 0 && tileSize % (1 << (maxLevel + 1)) == 0)
        maxLevel++;

    // --- Upload ---
    if (!texture) glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.x, size.y, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, maxLevel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenerateMipmap(GL_TEXTURE_2D);

    names.clear();
    indices.clear();
    for (int i = 0; i < count; i++) {
        names.push_back(images[i].name);
        indices[images[i].name] = i;
    }

    std::cout << "Texture atlas: " << count - 1 << " textures, " << size.x << "x" << size.y
              << " (" << tileSize << "px tiles, " << maxLevel << " mip levels)" << std::endl;
    return count > 1;
}

int TextureAtlas::GetIndex(const std::string& name) const {
    auto it = indices.find(name);
    return it != indices.end() ? it->second : kMissingIndex;
}

void TextureAtlas::Bind(GLenum textureUnit) const {
    glActiveTexture(textureUnit);
    glBindTexture(GL_TEXTURE_2D, texture);
}

void TextureAtlas::ApplyUniforms(Shader& shader) const {
    const glm::vec2 atlasSize(size);
    const float cell = static_cast<float>(tileSize + 2 * padding);
    shader.SetUniform("atlasColumns", columns);
    shader.SetUniform("atlasCellUV", glm::vec2(cell) / atlasSize);
    shader.SetUniform("atlasPaddingUV", glm::vec2(static_cast<float>(padding)) / atlasSize);
    shader.SetUniform("atlasTileUV", glm::vec2(static_cast<float>(tileSize)) / atlasSize);
}
