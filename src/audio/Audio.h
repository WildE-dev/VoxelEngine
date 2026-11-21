#pragma once
// Sound effects through OpenAL: short sounds played at a place in the world (heard
// from the listener's position, quieter with distance) or straight in the ears, and
// one looping ambience (rain). Sounds are 16-bit mono WAV files in Resources/Sounds,
// made by tools/make_sounds.py; "name" plays name.wav, or one of name_1.wav,
// name_2.wav, ... at random, at a slightly varied pitch.
//
// Without OpenAL (VOXEL_AUDIO not defined) everything here quietly does nothing.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "world/Block.h"

class Audio
{
public:
    Audio();
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    // Opens the default output device and loads every sound in `directory`. Returns
    // false (and stays silent) if there's no audio.
    bool Init(const std::string& directory);
    bool IsAvailable() const;

    // Where the listener is and which way they face (call every frame).
    void SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up);
    void SetVolume(float volume); // 0..1, everything

    // A sound at a place in the world (volume 0..1; pitch varied by up to `pitchSpread`).
    void Play(const std::string& name, const glm::vec3& position, float volume = 1.0f, float pitchSpread = 0.08f);
    // A sound in the listener's own ears (no distance, no direction).
    void PlayHere(const std::string& name, float volume = 1.0f, float pitchSpread = 0.05f);
    // The looping ambience: `name` at `volume` (0 stops it).
    void SetLoop(const std::string& name, float volume);

    // The sound family for a block: footsteps, mining and breaking go by what it's made of.
    static const char* Material(BlockType type); // "soft", "hard", "sand", "wood" or "glass"

    // A WAV file's samples (16-bit mono PCM only), for loading and for tests.
    struct Clip {
        std::vector<int16_t> samples;
        int rate = 0;
    };
    static bool LoadWav(const std::string& path, Clip& clip);
    // How many sounds were loaded (each variant counts).
    size_t LoadedCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
