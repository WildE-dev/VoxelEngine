// Chat and commands. A line starting with '/' (or any line typed into the console) is
// a command: the first word picks it, the rest are its arguments.
#include "server/Server.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <unordered_set>

#include "server/ServerWorld.h"
#include "server/WorldStorage.h"

namespace {

using Kind = net::ChatMessage::Kind;

// Printable text only: no control characters (they'd mess up the log or the console).
std::string Clean(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text)
        if (c >= 0x20 && c != 0x7F) out.push_back(static_cast<char>(c));
    const size_t first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return out.substr(first, out.find_last_not_of(' ') - first + 1);
}

std::vector<std::string> Words(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> words;
    for (std::string w; in >> w;) words.push_back(w);
    return words;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The words from `first` on, joined back with spaces.
std::string Rest(const std::vector<std::string>& words, size_t first) {
    std::string out;
    for (size_t i = first; i < words.size(); i++) out += (i > first ? " " : "") + words[i];
    return out;
}

bool ParseNumber(const std::string& text, double& value) {
    char* end = nullptr;
    value = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && std::isfinite(value);
}

// A coordinate, absolute or relative to `base` with a leading '~' ("~", "~5", "~-2").
bool ParseCoordinate(const std::string& text, float base, float& value) {
    double v = 0.0;
    if (!text.empty() && text[0] == '~') {
        if (text.size() > 1 && !ParseNumber(text.substr(1), v)) return false;
        value = base + static_cast<float>(v);
        return true;
    }
    if (!ParseNumber(text, v)) return false;
    value = static_cast<float>(v);
    return true;
}

std::string FormatTime(double timeOfDay) {
    const int minutes = static_cast<int>((timeOfDay * 24.0 + 6.0) * 60.0) % (24 * 60); // sunrise at 06:00
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d", minutes / 60, minutes % 60);
    return text;
}

// Who may run what.
enum class Level { Everyone, Operator, Console };

struct HelpLine {
    Level level;
    const char* text;
};
constexpr HelpLine kHelp[] = {
    { Level::Everyone, "/help - this list" },
    { Level::Everyone, "/list - who's online" },
    { Level::Everyone, "/me <action> - say what you're doing" },
    { Level::Everyone, "/msg <player> <text> - a private message" },
    { Level::Everyone, "/spawn - back to the spawn point" },
    { Level::Everyone, "/time, /weather - show the time and weather" },
    { Level::Everyone, "/ops - list the operators" },
    { Level::Operator, "/time <day|noon|dusk|night|midnight|dawn|HH:MM> - set the time of day" },
    { Level::Operator, "/weather <clear|rain|storm|auto> - set the weather" },
    { Level::Operator, "/tp <x> <y> <z> | /tp <player> | /tp <player> <x> <y> <z> - teleport (~ for relative)" },
    { Level::Operator, "/save - save the world now" },
    { Level::Operator, "/pressurise <x y z> - fill the air network there with air" },
    { Level::Operator, "/form <x y z> - form the structure a block is part of (as the engineer's hammer does)" },
    { Level::Operator, "/kick <player> [reason] - remove a player" },
    { Level::Operator, "/op <player>, /deop <player> - make or unmake an operator" },
    { Level::Operator, "/noclip [player] - fly through blocks, on or off (operators may also use the laser, L)" },
    { Level::Everyone, "/gamemode - show your game mode" },
    { Level::Operator, "/gamemode <creative|survival> [player] - creative: anything, unhurt; survival: collect what you build with" },
    { Level::Operator, "/give <block> [count] [player] - give survival blocks (count in whole blocks, default 64), or /give <tool|cap|tunic|boots> [player]" },
    { Level::Operator, "/fill <x1 y1 z1> <x2 y2 z2> <block> - fill a box with a block (air clears it; up to 32768 cells)" },
    { Level::Operator, "/summon <deer|wolf|bat|dwarf|crossbow_dwarf|dwarf_chief|dwarf_miner> [x y z] - bring a creature (default: a few blocks ahead of you)" },
    { Level::Operator, "/locate <spring|sinkhole|oil> - where the nearest spring (a lake's overflow), sinkhole (where its river ends) or oil is" },
    { Level::Everyone, "/horde - whether tonight is a horde night" },
    { Level::Operator, "/horde <on|off> - start or end a horde night" },
    { Level::Operator, "/creatures [clear] - count the creatures, or remove them all" },
    { Level::Everyone, "/food - how hungry you are" },
    { Level::Everyone, "/achievements - the milestones you've reached, and those still to come" },
    { Level::Operator, "/food <0-20> [player] - set how fed someone is (20: full)" },
    { Level::Everyone, "/pvp - whether players can hurt each other" },
    { Level::Operator, "/pvp <on|off> - let players hurt each other, or not" },
    { Level::Console, "/stop - shut the server down" },
};

// Commands that need more than being a player. (/time and /weather only to change
// things: showing them is for everyone.)
Level RequiredLevel(const std::string& command, size_t argumentCount) {
    if (command == "stop") return Level::Console;
    if (command == "tp" || command == "teleport" || command == "save" || command == "kick" || command == "op" || command == "deop" ||
        command == "noclip" || command == "give" || command == "fill" || command == "pressurise" || command == "locate" || command == "form" || command == "summon" || command == "creatures")
        return Level::Operator;
    if ((command == "time" || command == "weather" || command == "gamemode" || command == "pvp" || command == "food" || command == "horde") && argumentCount > 0)
        return Level::Operator;
    return Level::Everyone;
}

} // namespace

bool Server::IsOperatorName(const std::string& name) const {
    const std::string lower = Lower(name);
    return operators.count(lower) || (!options.ownerName.empty() && Lower(options.ownerName) == lower);
}

bool Server::IsOperator(const ClientState* sender) const {
    return !sender || IsOperatorName(sender->name);
}

bool Server::SetOperator(const std::string& name, bool op) {
    const std::string lower = Lower(Clean(name));
    if (lower.empty()) return false;
    const bool changed = op ? operators.insert(lower).second : operators.erase(lower) > 0;
    if (changed) SaveOperators();
    return changed;
}

void Server::LoadOperators() {
    if (!storage) return;
    std::ifstream file(storage->GetDirectory() / "ops.txt");
    for (std::string line; std::getline(file, line);) {
        line = Clean(line);
        if (!line.empty() && line[0] != '#') operators.insert(Lower(line));
    }
    if (!operators.empty()) Log(std::to_string(operators.size()) + (operators.size() == 1 ? " operator" : " operators") + " in ops.txt");
}

void Server::SaveOperators() const {
    if (!storage) return; // not saved: they last until the server stops
    namespace fs = std::filesystem;
    const fs::path path = storage->GetDirectory() / "ops.txt";
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::trunc);
        file << "# Operators: one player name per line (case doesn't matter).\n";
        std::vector<std::string> sorted(operators.begin(), operators.end());
        std::sort(sorted.begin(), sorted.end());
        for (const auto& name : sorted) file << name << "\n";
        if (!file) {
            std::cerr << "[server] can't write " << temp.string() << std::endl;
            return;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) std::cerr << "[server] can't write " << path.string() << ": " << ec.message() << std::endl;
}

void Server::QueueConsoleCommand(const std::string& line) {
    std::lock_guard<std::mutex> lock(consoleMutex);
    consoleQueue.push_back(line);
}

void Server::RunConsoleCommands() {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(consoleMutex);
        lines.swap(consoleQueue);
    }
    for (const auto& raw : lines) {
        std::string line = Clean(raw);
        if (line.empty()) continue;
        if (line[0] != '/') line = "/" + line; // the console takes commands with or without the slash
        RunCommand(line, nullptr, [](Kind kind, const std::string& text) {
            (kind == Kind::Error ? std::cerr : std::cout) << "[server] " << text << std::endl;
        });
    }
}

void Server::SendChat(ClientState& client, Kind kind, const std::string& text) {
    if (!client.welcomed) return;
    net.Send(client.peer, net::kChannelReliable, net::Encode(net::ChatMessage{ kind, text }));
}

void Server::Broadcast(Kind kind, const std::string& text) {
    Log(text);
    for (auto& [peer, client] : clients) SendChat(client, kind, text);
}

Server::ClientState* Server::FindPlayer(const std::string& name) {
    // Exact name first, then case-insensitive.
    for (auto& [peer, client] : clients)
        if (client.welcomed && client.name == name) return &client;
    for (auto& [peer, client] : clients)
        if (client.welcomed && Lower(client.name) == Lower(name)) return &client;
    return nullptr;
}

void Server::SendAbilities(ClientState& client) {
    uint8_t flags = 0;
    if (IsOperator(&client)) flags |= net::kAbilityOperator;
    if (client.noclip) flags |= net::kAbilityNoclip;
    net.Send(client.peer, net::kChannelReliable, net::Encode(net::Abilities{ flags }));
}

void Server::Teleport(ClientState& client, const glm::vec3& feet) {
    client.correctionId++;
    client.feet = client.lastValid = feet;
    client.blockedStates = 0;
    client.falling = false; // (a fall from here on counts from here)
    client.fallPeak = feet.y;
    net.Send(client.peer, net::kChannelReliable, net::Encode(net::Correction{ client.correctionId, feet }));
}

void Server::HandleChat(ClientState& client, const std::string& raw) {
    const std::string text = Clean(raw);
    if (text.empty()) return;
    if (text[0] == '/') {
        Log(client.name + " ran " + text);
        RunCommand(text, &client, [&](Kind kind, const std::string& reply) { SendChat(client, kind, reply); });
        return;
    }
    Broadcast(Kind::Chat, "<" + client.name + "> " + text);
}

void Server::RunCommand(const std::string& line, ClientState* sender, const Reply& reply) {
    const std::vector<std::string> words = Words(line.substr(1));
    if (words.empty()) return reply(Kind::Error, "Type /help for the list of commands");
    const std::string command = Lower(words[0]);
    const std::string who = sender ? sender->name : "Server";
    auto usage = [&](const char* text) { reply(Kind::Error, std::string("Usage: ") + text); };

    const Level level = !sender ? Level::Console : IsOperator(sender) ? Level::Operator : Level::Everyone;
    const Level needed = RequiredLevel(command, words.size() - 1);
    if (needed > level) {
        if (sender) Log(sender->name + " isn't allowed to run /" + command);
        return reply(Kind::Error, needed == Level::Console ? "/" + command + " only works from the server console"
                                                           : "Only operators can do that (" + line + ")");
    }

    if (command == "help" || command == "?") {
        for (const auto& h : kHelp)
            if (h.level <= level) reply(Kind::Info, h.text);
    }
    else if (command == "op" || command == "deop") {
        if (words.size() != 2) return usage(command == "op" ? "/op <player>" : "/deop <player>");
        const bool op = command == "op";
        ClientState* target = FindPlayer(words[1]);
        const std::string name = target ? target->name : words[1]; // offline players can be (un)made too
        if (!op && !options.ownerName.empty() && Lower(name) == Lower(options.ownerName))
            return reply(Kind::Error, name + " owns this server and is always an operator");
        if (!SetOperator(name, op)) return reply(Kind::Error, name + (op ? " is already an operator" : " isn't an operator"));
        Broadcast(Kind::Info, who + (op ? " made " : " removed ") + name + (op ? " an operator" : " as an operator"));
        if (target) {
            SendChat(*target, Kind::Info, op ? "You are now an operator (/help lists what you can do)" : "You are no longer an operator");
            if (!op) target->noclip = false; // no-clip is an operator's privilege
            SendAbilities(*target);
        }
    }
    else if (command == "noclip") {
        ClientState* target = words.size() > 1 ? FindPlayer(words[1]) : sender;
        if (words.size() > 1 && !target) return reply(Kind::Error, "No player called " + words[1]);
        if (!target) return usage("noclip <player>");
        target->noclip = !target->noclip;
        SendAbilities(*target);
        const std::string state = target->noclip ? "on" : "off";
        if (target != sender) SendChat(*target, Kind::Info, who + " turned no-clip " + state + " for you");
        reply(Kind::Info, "No-clip " + state + (target != sender ? " for " + target->name : ""));
    }
    else if (command == "gamemode" || command == "gm") {
        auto name = [](net::GameMode m) { return m == net::GameMode::Creative ? "creative" : "survival"; };
        if (words.size() < 2) {
            if (!sender) return usage("gamemode <creative|survival> <player>");
            return reply(Kind::Info, std::string("You're in ") + name(sender->mode) + " mode");
        }
        const std::string arg = Lower(words[1]);
        net::GameMode mode;
        if (arg == "creative" || arg == "c" || arg == "1") mode = net::GameMode::Creative;
        else if (arg == "survival" || arg == "s" || arg == "0") mode = net::GameMode::Survival;
        else return usage("/gamemode <creative|survival> [player]");
        ClientState* target = words.size() > 2 ? FindPlayer(words[2]) : sender;
        if (words.size() > 2 && !target) return reply(Kind::Error, "No player called " + words[2]);
        if (!target) return usage("gamemode <creative|survival> <player>");
        target->mode = mode;
        if (mode == net::GameMode::Creative) target->health = kMaxHealth; // creative heals
        SendMode(*target);
        SendHealth(*target, false);
        if (target != sender) SendChat(*target, Kind::Info, who + " put you in " + name(mode) + " mode");
        reply(Kind::Info, std::string(target == sender ? "You're" : target->name + " is") + " now in " + name(mode) + " mode");
    }
    else if (command == "give") {
        if (words.size() < 2) return usage("/give <block|tool> [count] [player]");
        // A tool: theirs to keep.
        if (const Tool tool = Mining::ToolFromId(Lower(words[1])); tool != Tool::None) {
            ClientState* target = words.size() > 2 ? FindPlayer(words.back()) : sender;
            if (!target) return usage("give <tool> <player>");
            target->tools |= Crafting::ToolBit(tool);
            SendResources(*target);
            if (target != sender) SendChat(*target, Kind::Info, who + " gave you a " + Mining::ToolId(tool));
            return reply(Kind::Info, std::string("Gave a ") + Mining::ToolId(tool) + (target != sender ? " to " + target->name : ""));
        }
        // A piece of armour: worn from now on.
        if (const ArmourPiece piece = Armour::FromId(Lower(words[1])); piece != ArmourPiece::Count) {
            ClientState* target = words.size() > 2 ? FindPlayer(words.back()) : sender;
            if (!target) return usage("give <cap|tunic|boots> <player>");
            target->armour |= Armour::Bit(piece);
            SendResources(*target);
            if (target != sender) SendChat(*target, Kind::Info, who + " gave you a " + Armour::Name(piece));
            return reply(Kind::Info, std::string("Gave a ") + Armour::Name(piece) + (target != sender ? " to " + target->name : ""));
        }
        const BlockType type = BlockFromId(Lower(words[1]));
        if (type == BlockType::COUNT || Block(type).IsLiquid())
            return reply(Kind::Error, "No block called " + words[1] + " (e.g. dirt, stone, stone_brick, sand, log)");
        double count = 64.0;
        if (words.size() > 2 && (!ParseNumber(words[2], count) || count <= 0.0 || count > 1e6)) return usage("/give <block> [count] [player]");
        ClientState* target = words.size() > 3 ? FindPlayer(words[3]) : sender;
        if (words.size() > 3 && !target) return reply(Kind::Error, "No player called " + words[3]);
        if (!target) return usage("give <block> <count> <player>");
        target->resources[static_cast<size_t>(type)] += static_cast<uint32_t>(count * Block::kUnitsPerBlock + 0.5);
        SendResources(*target);
        char amount[32];
        std::snprintf(amount, sizeof amount, "%g", count);
        if (target != sender) SendChat(*target, Kind::Info, who + " gave you " + amount + " " + BlockId(type));
        reply(Kind::Info, std::string("Gave ") + amount + " " + BlockId(type) + (target != sender ? " to " + target->name : ""));
    }
    else if (command == "fill") {
        if (words.size() != 8) return usage("/fill <x1 y1 z1> <x2 y2 z2> <block>");
        double v[6];
        for (int i = 0; i < 6; i++)
            if (!ParseNumber(words[1 + i], v[i]) || std::abs(v[i]) > 1e7) return usage("/fill <x1 y1 z1> <x2 y2 z2> <block>");
        const BlockType type = Lower(words[7]) == "air" ? BlockType::AIR : BlockFromId(Lower(words[7]));
        if (type == BlockType::COUNT) return reply(Kind::Error, "No block called " + words[7]);
        const glm::ivec3 a(glm::floor(glm::dvec3(v[0], v[1], v[2]))), b(glm::floor(glm::dvec3(v[3], v[4], v[5])));
        const glm::ivec3 lo = glm::min(a, b), hi = glm::max(a, b), size = hi - lo + 1;
        if (static_cast<long long>(size.x) * size.y * size.z > 32768) return reply(Kind::Error, "That's too big a box (32768 cells at most)");
        const Block block = type == BlockType::WATER ? Block::Water(Block::kMaxWaterVolume) : Block(type);
        for (int x = lo.x; x <= hi.x; x++)
            for (int y = lo.y; y <= hi.y; y++)
                for (int z = lo.z; z <= hi.z; z++) {
                    Block old;
                    if (!world->GetBlock(x, y, z, old)) continue;
                    if (IsStorageType(old.type)) RemoveChest({ x, y, z }, nullptr);
                    world->SetBlock(x, y, z, block);
                    if (IsStorageType(type)) chests[{ x, y, z }] = ChestItems{};
                }
        reply(Kind::Info, "Filled " + std::to_string(size.x * size.y * size.z) + " cells with " + BlockId(type));
    }
    else if (command == "form") {
        double v[3];
        if (words.size() != 4) return usage("/form <x y z>");
        for (int i = 0; i < 3; i++)
            if (!ParseNumber(words[1 + i], v[i]) || std::abs(v[i]) > 1e7) return usage("/form <x y z>");
        TryForm(sender, glm::ivec3(glm::floor(glm::dvec3(v[0], v[1], v[2]))));
    }
    else if (command == "pressurise") {
        double v[3];
        if (words.size() != 4) return usage("/pressurise <x y z>");
        for (int i = 0; i < 3; i++)
            if (!ParseNumber(words[1 + i], v[i]) || std::abs(v[i]) > 1e7) return usage("/pressurise <x y z>");
        const glm::ivec3 start(glm::floor(glm::dvec3(v[0], v[1], v[2])));
        std::lock_guard<std::recursive_mutex> edit(world->GetEditMutex());
        // Every part joined to it, full (the next air update shares it out).
        std::vector<glm::ivec3> parts;
        std::unordered_set<glm::ivec3, Coords::IVec3Hash> seen;
        auto partAt = [&](const glm::ivec3& p, Block& b) { return world->GetBlock(p.x, p.y, p.z, b) && Power::IsAirPart(b.type); };
        if (Block b; partAt(start, b)) parts.push_back(start), seen.insert(start);
        for (size_t i = 0; i < parts.size() && parts.size() < static_cast<size_t>(Power::kMaxNetwork); i++)
            for (int f = 0; f < kFaceCount; f++) {
                const glm::ivec3 n = parts[i] + glm::ivec3(kFaceOffsets[f][0], kFaceOffsets[f][1], kFaceOffsets[f][2]);
                Block b;
                if (!seen.count(n) && partAt(n, b)) parts.push_back(n), seen.insert(n);
            }
        if (parts.empty()) return reply(Kind::Error, "There's no air pipe or machine there");
        for (const auto& p : parts) {
            Block b;
            world->GetBlock(p.x, p.y, p.z, b);
            air[p] = Power::CapacityOf(b.type);
        }
        reply(Kind::Info, "Filled " + std::to_string(parts.size()) + " air parts");
    }
    else if (command == "horde") {
        if (words.size() == 1) return reply(Kind::Info, creatures.Horde() ? "Tonight is a horde night" : "No horde tonight");
        const std::string arg = Lower(words[1]);
        if (arg != "on" && arg != "off") return usage("/horde [on|off]");
        if ((arg == "on") == creatures.Horde()) return reply(Kind::Info, creatures.Horde() ? "A horde night is already on" : "There's no horde night on");
        SetHorde(arg == "on");
    }
    else if (command == "locate") {
        const std::string what = words.size() == 2 ? Lower(words[1]) : "";
        if (what != "spring" && what != "sinkhole" && what != "oil") return usage("/locate <spring|sinkhole|oil>");
        if (what == "oil") {
            const glm::vec3 at = sender ? sender->feet : glm::vec3(0.0f);
            const auto oil = world->GetTerrain().OilNear(static_cast<int>(std::floor(at.x)), static_cast<int>(std::floor(at.z)), 600);
            if (!oil) return reply(Kind::Info, "No oil within 600 blocks");
            return reply(Kind::Info, "There's oil shale under " + std::to_string(oil->x) + " " + std::to_string(oil->y) + " (between " +
                                         std::to_string(TerrainGenerator::kOilBottom) + " and " + std::to_string(TerrainGenerator::kOilTop) + "), " +
                                         std::to_string(static_cast<int>(glm::distance(glm::vec2(at.x, at.z), glm::vec2(*oil)))) + " blocks away");
        }
        const glm::vec3 from = sender ? sender->feet : glm::vec3(0.0f);
        const glm::ivec3 c = Coords::ChunkOf(glm::ivec3(glm::floor(from)));
        const TerrainGenerator& terrain = world->GetTerrain();
        std::optional<glm::ivec2> best;
        float bestDistance = 1e9f;
        for (int dx = -24; dx <= 24; dx++)
            for (int dz = -24; dz <= 24; dz++)
                for (const glm::ivec2& sp : what == "spring" ? terrain.SpringsIn(c.x + dx, c.z + dz) : terrain.DrainsIn(c.x + dx, c.z + dz)) {
                    const float d = glm::distance(glm::vec2(from.x, from.z), glm::vec2(sp) + 0.5f);
                    if (d < bestDistance) bestDistance = d, best = sp;
                }
        if (!best) return reply(Kind::Info, "No " + what + " within 768 blocks");
        const auto column = terrain.ColumnAt(best->x, best->y);
        reply(Kind::Info, "The nearest " + what + " is at " + std::to_string(best->x) + " " + std::to_string(column.surfaceRow) + " " + std::to_string(best->y) + ", " +
                              std::to_string(static_cast<int>(bestDistance)) + " blocks away");
    }
    else if (command == "summon") {
        const CreatureType type = words.size() > 1 ? CreatureFromId(Lower(words[1])) : CreatureType::Count;
        if (type == CreatureType::Count) return usage("/summon <deer|wolf|bat|dwarf|crossbow_dwarf|dwarf_chief|dwarf_miner> [x y z]");
        glm::vec3 at;
        if (words.size() >= 5) {
            const glm::vec3 base = sender ? sender->feet : glm::vec3(0.0f);
            for (int a = 0; a < 3; a++) {
                double v = 0.0;
                const std::string& w = words[2 + a];
                const bool relative = !w.empty() && w[0] == '~';
                if ((relative && w.size() > 1 && !ParseNumber(w.substr(1), v)) || (!relative && !ParseNumber(w, v)))
                    return usage("/summon <deer|wolf|bat|dwarf|crossbow_dwarf|dwarf_chief|dwarf_miner> [x y z]");
                at[a] = static_cast<float>(relative ? base[a] + v : v);
            }
        }
        else if (sender) {
            const float yaw = glm::radians(sender->yaw);
            at = sender->feet + glm::vec3(std::cos(yaw), 0.0f, std::sin(yaw)) * 4.0f + glm::vec3(0.0f, 0.5f, 0.0f);
        }
        else return usage("summon <deer|ghoul|skeleton> <x> <y> <z>");
        if (!creatures.Spawn(type, at)) return reply(Kind::Error, "Too many creatures already");
        char where[64];
        std::snprintf(where, sizeof where, "%.1f, %.1f, %.1f", at.x, at.y, at.z);
        reply(Kind::Info, std::string("Summoned a ") + SpecOf(type).id + " at " + where);
    }
    else if (command == "creatures") {
        if (words.size() > 1 && Lower(words[1]) == "clear") {
            const size_t n = creatures.Count();
            creatures.Clear();
            return reply(Kind::Info, "Removed " + std::to_string(n) + " creatures");
        }
        int counts[static_cast<int>(CreatureType::Count)] = {};
        for (const auto& c : creatures.All()) counts[static_cast<int>(c.type)]++;
        std::string text = std::to_string(creatures.Count()) + " creatures";
        for (int t = 0; t < static_cast<int>(CreatureType::Count); t++)
            text += std::string(t ? ", " : ": ") + std::to_string(counts[t]) + " " + SpecOf(static_cast<CreatureType>(t)).id;
        reply(Kind::Info, text);
    }
    else if (command == "achievements" || command == "ach") {
        if (!sender) return reply(Kind::Error, "Only players have achievements");
        int earned = 0;
        std::string have, todo;
        for (int i = 0; i < static_cast<int>(Achievement::Count); i++) {
            const Achievement a = static_cast<Achievement>(i);
            if (sender->achievements & Achievements::Bit(a)) {
                earned++;
                have += (have.empty() ? "" : ", ") + std::string(Achievements::Name(a));
            }
            else todo += (todo.empty() ? "" : "; ") + std::string(Achievements::Description(a));
        }
        reply(Kind::Info, std::to_string(earned) + " of " + std::to_string(static_cast<int>(Achievement::Count)) + " achievements" + (have.empty() ? "" : ": " + have));
        if (!todo.empty()) reply(Kind::Info, "Still to do: " + todo);
    }
    else if (command == "food") {
        if (words.size() == 1) {
            if (!sender) return usage("food <0-20> <player>");
            return reply(Kind::Info, "Food: " + std::to_string(sender->food) + "/" + std::to_string(Hunger::kMaxFood));
        }
        double points = 0.0;
        if (!ParseNumber(words[1], points) || points < 0.0 || points > Hunger::kMaxFood) return usage("/food <0-20> [player]");
        ClientState* target = words.size() > 2 ? FindPlayer(words[2]) : sender;
        if (!target) return reply(Kind::Error, words.size() > 2 ? "No player called " + words[2] : "Name a player");
        target->food = static_cast<int>(points);
        target->exhaustion = 0.0f;
        SendHealth(*target, false);
        reply(Kind::Info, "Set " + target->name + "'s food to " + std::to_string(target->food));
    }
    else if (command == "pvp") {
        if (words.size() == 1) return reply(Kind::Info, std::string("PvP is ") + (options.pvp ? "on" : "off"));
        const std::string w = Lower(words[1]);
        if (w != "on" && w != "off") return usage("/pvp [on|off]");
        options.pvp = w == "on";
        Broadcast(Kind::Info, std::string("PvP is now ") + (options.pvp ? "on: players can hurt each other" : "off"));
    }
    else if (command == "ops") {
        std::vector<std::string> names(operators.begin(), operators.end());
        if (!options.ownerName.empty() && !operators.count(Lower(options.ownerName))) names.push_back(Lower(options.ownerName));
        std::sort(names.begin(), names.end());
        std::string list;
        for (const auto& n : names) list += (list.empty() ? "" : ", ") + n;
        reply(Kind::Info, names.empty() ? "No operators (the console can add some with op <name>)" : "Operators: " + list);
    }
    else if (command == "list" || command == "who") {
        std::string names;
        int count = 0;
        for (const auto& [peer, client] : clients)
            if (client.welcomed) names += (count++ ? ", " : "") + client.name;
        reply(Kind::Info, std::to_string(count) + (count == 1 ? " player online" : " players online") + (count ? ": " + names : ""));
    }
    else if (command == "me") {
        if (words.size() < 2) return usage("/me <action>");
        Broadcast(Kind::Chat, "* " + who + " " + Rest(words, 1));
    }
    else if (command == "msg" || command == "tell" || command == "w") {
        if (words.size() < 3) return usage("/msg <player> <text>");
        ClientState* target = FindPlayer(words[1]);
        if (!target) return reply(Kind::Error, "No player called " + words[1]);
        const std::string text = Rest(words, 2);
        SendChat(*target, Kind::Private, who + " whispers: " + text);
        reply(Kind::Private, "You whisper to " + target->name + ": " + text);
    }
    else if (command == "time") {
        if (words.size() < 2) return reply(Kind::Info, "It's " + FormatTime(timeOfDay));
        const std::string arg = Lower(words[1] == "set" && words.size() > 2 ? words[2] : words[1]);
        double t;
        if (arg == "day" || arg == "morning") t = 0.05;
        else if (arg == "noon") t = 0.25;
        else if (arg == "dusk" || arg == "evening" || arg == "sunset") t = 0.48;
        else if (arg == "night") t = 0.6;
        else if (arg == "midnight") t = 0.75;
        else if (arg == "dawn" || arg == "sunrise") t = 0.98;
        else {
            // HH:MM, with sunrise at 06:00.
            const size_t colon = arg.find(':');
            double hours = 0.0, minutes = 0.0;
            if (colon == std::string::npos || !ParseNumber(arg.substr(0, colon), hours) || !ParseNumber(arg.substr(colon + 1), minutes) ||
                hours < 0 || hours >= 24 || minutes < 0 || minutes >= 60)
                return usage("/time [day|noon|dusk|night|midnight|dawn|HH:MM]");
            t = (hours + minutes / 60.0 - 6.0) / 24.0;
        }
        SetTimeOfDay(t);
        Broadcast(Kind::Info, who + " set the time to " + FormatTime(timeOfDay));
    }
    else if (command == "weather") {
        auto name = [](Options::Weather w) {
            return w == Options::Weather::Clear ? "clear" : w == Options::Weather::Rain ? "rain" : w == Options::Weather::Storm ? "storm" : "auto";
        };
        if (words.size() < 2)
            return reply(Kind::Info, std::string(storm && raining ? "There's a thunderstorm" : raining ? "It's raining" : "It's clear") + " (weather: " + name(weatherMode) + ")");
        const std::string arg = Lower(words[1]);
        if (arg == "clear" || arg == "sun") weatherMode = Options::Weather::Clear;
        else if (arg == "rain") weatherMode = Options::Weather::Rain;
        else if (arg == "storm" || arg == "thunder") weatherMode = Options::Weather::Storm;
        else if (arg == "auto") {
            weatherMode = Options::Weather::Auto;
            weatherTimer = raining ? 3.0 * 60.0 : 15.0 * 60.0; // carry on with the current weather for a while
        }
        else return usage("/weather [clear|rain|storm|auto]");
        Broadcast(Kind::Info, who + " set the weather to " + name(weatherMode));
    }
    else if (command == "tp" || command == "teleport") {
        ClientState* target = sender;
        glm::vec3 to;
        size_t coords = 0; // index of the first coordinate, if any
        if (words.size() == 4) coords = 1;           // tp x y z
        else if (words.size() == 5) {                // tp player x y z
            target = FindPlayer(words[1]);
            if (!target) return reply(Kind::Error, "No player called " + words[1]);
            coords = 2;
        }
        else if (words.size() == 2) {                // tp player
            ClientState* other = FindPlayer(words[1]);
            if (!other) return reply(Kind::Error, "No player called " + words[1]);
            if (!sender) return reply(Kind::Error, "The console has no position: use /tp <player> <x> <y> <z>");
            to = other->feet;
        }
        else return usage("/tp <x> <y> <z> | /tp <player> | /tp <player> <x> <y> <z>");
        if (!target) return reply(Kind::Error, "The console can't teleport itself: name a player");
        if (coords) {
            if (!ParseCoordinate(words[coords], target->feet.x, to.x) || !ParseCoordinate(words[coords + 1], target->feet.y, to.y) ||
                !ParseCoordinate(words[coords + 2], target->feet.z, to.z))
                return reply(Kind::Error, "Coordinates must be numbers, or ~ for relative");
            if (std::abs(to.x) > 1e6f || std::abs(to.y) > 1e4f || std::abs(to.z) > 1e6f) return reply(Kind::Error, "That's too far away");
        }
        Teleport(*target, to);
        char where[64];
        std::snprintf(where, sizeof where, "%.1f, %.1f, %.1f", to.x, to.y, to.z);
        if (target != sender) SendChat(*target, Kind::Info, who + " teleported you to " + where);
        reply(Kind::Info, "Teleported " + (target == sender ? std::string("to ") : target->name + " to ") + where);
    }
    else if (command == "spawn") {
        if (!sender) return reply(Kind::Error, "Only a player can go to spawn");
        Teleport(*sender, SpawnPoint());
        reply(Kind::Info, "Back to spawn");
    }
    else if (command == "save") {
        if (!storage) return reply(Kind::Error, "This world isn't being saved (--no-save)");
        Save(false);
        reply(Kind::Info, "World saved");
    }
    else if (command == "kick") {
        if (words.size() < 2) return usage("/kick <player> [reason]");
        ClientState* target = FindPlayer(words[1]);
        if (!target) return reply(Kind::Error, "No player called " + words[1]);
        if (target == sender) return reply(Kind::Error, "You can't kick yourself");
        const std::string name = target->name;
        Kick(*target, words.size() > 2 ? Rest(words, 2) : "kicked by " + who);
        reply(Kind::Info, "Kicked " + name);
    }
    else if (command == "stop") {
        Broadcast(Kind::Info, "The server is stopping");
        running = false;
    }
    else {
        reply(Kind::Error, "Unknown command /" + words[0] + " (type /help)");
    }
}
