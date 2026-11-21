#include "audio/Audio.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>

#ifdef VOXEL_AUDIO
#ifdef __APPLE__
#define OPENAL_DEPRECATED // (Apple's OpenAL is deprecated but present and working)
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif
#endif

namespace fs = std::filesystem;

const char* Audio::Material(BlockType type) {
    switch (type) {
    case BlockType::STONE:
    case BlockType::STONE_BRICK:
    case BlockType::MOSSY_BRICK:
    case BlockType::CRUMBLING_BRICK:
    case BlockType::LAMP:
    case BlockType::PUMP:
    case BlockType::PIPE:
    case BlockType::AIR_PIPE:
    case BlockType::DRILL:
    case BlockType::COMPRESSOR:
    case BlockType::SLUICE:
    case BlockType::PRESSURE_PLATE:
    case BlockType::SPIKES:
    case BlockType::ICE:
    case BlockType::PEBBLES:
    case BlockType::COAL_ORE:
    case BlockType::IRON_ORE: return "hard";
    case BlockType::SAND:
    case BlockType::SNOW: return "sand";
    case BlockType::LOG:
    case BlockType::PLANKS:
    case BlockType::WATER_WHEEL:
    case BlockType::SHAFT:
    case BlockType::CHEST:
    case BlockType::BED:
    case BlockType::LADDER:
    case BlockType::CAMPFIRE:
    case BlockType::SIGN:
    case BlockType::DOOR:
    case BlockType::DOOR_OPEN:
    case BlockType::CACTUS: return "wood";
    case BlockType::GLASS: return "glass";
    default: return "soft"; // grass, dirt, leaves, plants
    }
}

bool Audio::LoadWav(const std::string& path, Clip& clip) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, &data[at], 4); return v; };
    auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, &data[at], 2); return v; };
    if (data.size() < 44 || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) return false;
    // Walk the chunks for "fmt " and "data".
    int channels = 0, bits = 0;
    size_t at = 12;
    while (at + 8 <= data.size()) {
        const uint32_t size = u32(at + 4);
        if (at + 8 + size > data.size()) return false;
        if (std::memcmp(&data[at], "fmt ", 4) == 0 && size >= 16) {
            if (u16(at + 8) != 1) return false; // PCM
            channels = u16(at + 10);
            clip.rate = static_cast<int>(u32(at + 12));
            bits = u16(at + 22);
        }
        else if (std::memcmp(&data[at], "data", 4) == 0) {
            if (channels != 1 || bits != 16 || clip.rate <= 0) return false;
            clip.samples.resize(size / 2);
            std::memcpy(clip.samples.data(), &data[at + 8], clip.samples.size() * 2);
            return !clip.samples.empty();
        }
        at += 8 + size + (size & 1);
    }
    return false;
}

#ifdef VOXEL_AUDIO

struct Audio::Impl {
    ALCdevice* device = nullptr;
    ALCcontext* context = nullptr;
    std::map<std::string, std::vector<ALuint>> sounds; // name -> its variants' buffers
    std::vector<ALuint> sources;                        // a pool for one-shot sounds
    ALuint loopSource = 0;
    std::string loopName;
    std::mt19937 rng{ 7 };
    size_t next = 0;
    float master = 1.0f, loopVolume = 0.0f;

    ALuint Buffer(const std::string& name) {
        auto it = sounds.find(name);
        if (it == sounds.end() || it->second.empty()) return 0;
        return it->second[std::uniform_int_distribution<size_t>(0, it->second.size() - 1)(rng)];
    }
    // A source that isn't playing (or the oldest one, if they all are).
    ALuint FreeSource() {
        for (size_t i = 0; i < sources.size(); i++) {
            const ALuint s = sources[(next + i) % sources.size()];
            ALint state;
            alGetSourcei(s, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING) {
                next = (next + i + 1) % sources.size();
                return s;
            }
        }
        const ALuint s = sources[next];
        next = (next + 1) % sources.size();
        alSourceStop(s);
        return s;
    }
};

Audio::Audio() : impl(std::make_unique<Impl>()) {}

Audio::~Audio() {
    if (!impl->context) return;
    alSourceStopv(static_cast<ALsizei>(impl->sources.size()), impl->sources.data());
    alDeleteSources(static_cast<ALsizei>(impl->sources.size()), impl->sources.data());
    if (impl->loopSource) {
        alSourceStop(impl->loopSource);
        alDeleteSources(1, &impl->loopSource);
    }
    for (auto& [name, buffers] : impl->sounds) alDeleteBuffers(static_cast<ALsizei>(buffers.size()), buffers.data());
    alcMakeContextCurrent(nullptr);
    alcDestroyContext(impl->context);
    alcCloseDevice(impl->device);
}

bool Audio::Init(const std::string& directory) {
    impl->device = alcOpenDevice(nullptr);
    if (!impl->device) {
        std::cerr << "[audio] no output device: playing without sound" << std::endl;
        return false;
    }
    impl->context = alcCreateContext(impl->device, nullptr);
    if (!impl->context || !alcMakeContextCurrent(impl->context)) {
        std::cerr << "[audio] couldn't start OpenAL: playing without sound" << std::endl;
        if (impl->context) alcDestroyContext(impl->context);
        alcCloseDevice(impl->device);
        impl->context = nullptr;
        impl->device = nullptr;
        return false;
    }
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (entry.path().extension() != ".wav") continue;
        Clip clip;
        if (!LoadWav(entry.path().string(), clip)) {
            std::cerr << "[audio] can't read " << entry.path().string() << std::endl;
            continue;
        }
        // "step_soft_2" is a variant of "step_soft".
        std::string name = entry.path().stem().string();
        const size_t underscore = name.rfind('_');
        if (underscore != std::string::npos && underscore + 1 < name.size() &&
            std::all_of(name.begin() + underscore + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            name = name.substr(0, underscore);
        ALuint buffer;
        alGenBuffers(1, &buffer);
        alBufferData(buffer, AL_FORMAT_MONO16, clip.samples.data(), static_cast<ALsizei>(clip.samples.size() * 2), clip.rate);
        impl->sounds[name].push_back(buffer);
    }
    impl->sources.resize(24);
    alGenSources(static_cast<ALsizei>(impl->sources.size()), impl->sources.data());
    for (ALuint s : impl->sources) {
        alSourcef(s, AL_REFERENCE_DISTANCE, 3.0f);
        alSourcef(s, AL_MAX_DISTANCE, 40.0f);
        alSourcef(s, AL_ROLLOFF_FACTOR, 1.2f);
    }
    alGenSources(1, &impl->loopSource);
    alSourcei(impl->loopSource, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourcei(impl->loopSource, AL_LOOPING, AL_TRUE);
    alSource3f(impl->loopSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
    std::cout << "[audio] " << LoadedCount() << " sounds loaded" << std::endl;
    return true;
}

bool Audio::IsAvailable() const { return impl->context != nullptr; }

size_t Audio::LoadedCount() const {
    size_t n = 0;
    for (const auto& [name, buffers] : impl->sounds) n += buffers.size();
    return n;
}

void Audio::SetListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    if (!impl->context) return;
    alListener3f(AL_POSITION, position.x, position.y, position.z);
    const float orientation[6] = { forward.x, forward.y, forward.z, up.x, up.y, up.z };
    alListenerfv(AL_ORIENTATION, orientation);
}

void Audio::SetVolume(float volume) {
    // Applied to each sound as it's played (and to the loop now): a listener gain alone
    // isn't honoured by every OpenAL.
    const float v = std::clamp(volume, 0.0f, 1.0f);
    if (v == impl->master) return;
    impl->master = v;
    if (impl->context) alSourcef(impl->loopSource, AL_GAIN, impl->loopVolume * v);
}

void Audio::Play(const std::string& name, const glm::vec3& position, float volume, float pitchSpread) {
    if (!impl->context) return;
    const ALuint buffer = impl->Buffer(name);
    if (!buffer) return;
    const ALuint s = impl->FreeSource();
    alSourcei(s, AL_BUFFER, static_cast<ALint>(buffer));
    alSourcei(s, AL_SOURCE_RELATIVE, AL_FALSE);
    alSource3f(s, AL_POSITION, position.x, position.y, position.z);
    alSourcef(s, AL_GAIN, volume * impl->master);
    alSourcef(s, AL_PITCH, 1.0f + std::uniform_real_distribution<float>(-pitchSpread, pitchSpread)(impl->rng));
    alSourcePlay(s);
}

void Audio::PlayHere(const std::string& name, float volume, float pitchSpread) {
    if (!impl->context) return;
    const ALuint buffer = impl->Buffer(name);
    if (!buffer) return;
    const ALuint s = impl->FreeSource();
    alSourcei(s, AL_BUFFER, static_cast<ALint>(buffer));
    alSourcei(s, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(s, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcef(s, AL_GAIN, volume * impl->master);
    alSourcef(s, AL_PITCH, 1.0f + std::uniform_real_distribution<float>(-pitchSpread, pitchSpread)(impl->rng));
    alSourcePlay(s);
}

void Audio::SetLoop(const std::string& name, float volume) {
    if (!impl->context) return;
    ALint state;
    alGetSourcei(impl->loopSource, AL_SOURCE_STATE, &state);
    if (volume <= 0.001f || impl->master <= 0.001f) {
        if (state == AL_PLAYING) alSourceStop(impl->loopSource);
        return;
    }
    if (name != impl->loopName || state != AL_PLAYING) {
        const ALuint buffer = impl->Buffer(name);
        if (!buffer) return;
        alSourceStop(impl->loopSource);
        alSourcei(impl->loopSource, AL_BUFFER, static_cast<ALint>(buffer));
        alSourcePlay(impl->loopSource);
        impl->loopName = name;
    }
    impl->loopVolume = volume;
    alSourcef(impl->loopSource, AL_GAIN, volume * impl->master);
}

#else // no audio: everything does nothing

struct Audio::Impl {};
Audio::Audio() : impl(std::make_unique<Impl>()) {}
Audio::~Audio() = default;
bool Audio::Init(const std::string&) { return false; }
bool Audio::IsAvailable() const { return false; }
size_t Audio::LoadedCount() const { return 0; }
void Audio::SetListener(const glm::vec3&, const glm::vec3&, const glm::vec3&) {}
void Audio::SetVolume(float) {}
void Audio::Play(const std::string&, const glm::vec3&, float, float) {}
void Audio::PlayHere(const std::string&, float, float) {}
void Audio::SetLoop(const std::string&, float) {}

#endif
