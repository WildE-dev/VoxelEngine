#pragma once
// A name for a new player, made up the first time the game is opened (kept in the
// settings after that, and changeable in the menu): two words and a number, such as
// "BraveOtter42". Servers turn away a second player with a name that's already in use.
#include <chrono>
#include <cstdint>
#include <random>
#include <string>

namespace PlayerNames {

inline std::string Random(uint32_t seed) {
    static const char* const kFirst[] = { "Brave", "Quiet", "Swift", "Mossy", "Rusty", "Amber", "Clever", "Gentle", "Bold", "Lucky",
                                          "Sly", "Stony", "Misty", "Sunny", "Frosty", "Copper", "Iron", "Wild", "Merry", "Dusty" };
    static const char* const kSecond[] = { "Otter", "Badger", "Fox", "Heron", "Wolf", "Miner", "Mason", "Lark", "Beetle", "Hare",
                                           "Owl", "Pike", "Raven", "Stag", "Wren", "Smith", "Tinker", "Ferret", "Marten", "Finch" };
    std::mt19937 rng(seed);
    auto pick = [&](const char* const* words, size_t count) { return std::string(words[std::uniform_int_distribution<size_t>(0, count - 1)(rng)]); };
    const std::string first = pick(kFirst, std::size(kFirst)), second = pick(kSecond, std::size(kSecond));
    return first + second + std::to_string(std::uniform_int_distribution<int>(10, 99)(rng));
}

inline std::string Random() {
    std::random_device device;
    return Random(device() ^ static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
}

} // namespace PlayerNames
