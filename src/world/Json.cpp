#include "world/Json.h"

#include <fstream>
#include <sstream>

namespace Json {

bool ParseFile(const std::string& path, Value& out, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = path + ": can't open it";
        return false;
    }
    std::stringstream text;
    text << file.rdbuf();
    const std::string contents = text.str();
    Parser parser(contents);
    if (!parser.Parse(out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

} // namespace Json
