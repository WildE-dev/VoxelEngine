#include "world/Json.h"

#include <cstdio>
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

namespace {

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out + "\"";
}

std::string NumberText(double n) {
    char text[32];
    std::snprintf(text, sizeof text, "%.4f", n);
    std::string s = text;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

// The value on one line.
std::string Line(const Value& v) {
    switch (v.type) {
    case Value::Type::Null: return "null";
    case Value::Type::Bool: return v.boolean ? "true" : "false";
    case Value::Type::Number: return NumberText(v.number);
    case Value::Type::String: return Quote(v.string);
    case Value::Type::Array: {
        std::string s = "[";
        for (size_t i = 0; i < v.array.size(); i++) s += (i ? ", " : "") + Line(v.array[i]);
        return s + "]";
    }
    case Value::Type::Object: {
        if (v.object.empty()) return "{}";
        std::string s = "{ ";
        for (size_t i = 0; i < v.object.size(); i++) s += (i ? ", " : "") + Quote(v.object[i].first) + ": " + Line(v.object[i].second);
        return s + " }";
    }
    }
    return "null";
}

void Spread(const Value& v, int indent, int width, std::string& out) {
    const std::string line = Line(v);
    if (static_cast<int>(line.size()) + indent <= width || (v.type != Value::Type::Array && v.type != Value::Type::Object)) {
        out += line;
        return;
    }
    const std::string pad(static_cast<size_t>(indent) + 2, ' ');
    if (v.type == Value::Type::Array) {
        out += "[\n";
        for (size_t i = 0; i < v.array.size(); i++) {
            out += pad;
            Spread(v.array[i], indent + 2, width, out);
            out += i + 1 < v.array.size() ? ",\n" : "\n";
        }
        out += std::string(static_cast<size_t>(indent), ' ') + "]";
        return;
    }
    out += "{\n";
    for (size_t i = 0; i < v.object.size(); i++) {
        out += pad + Quote(v.object[i].first) + ": ";
        Spread(v.object[i].second, indent + 2, width, out);
        out += i + 1 < v.object.size() ? ",\n" : "\n";
    }
    out += std::string(static_cast<size_t>(indent), ' ') + "}";
}

} // namespace

std::string Write(const Value& v, int width) {
    std::string out;
    Spread(v, 0, width, out);
    return out + "\n";
}

std::string LeadingComments(const std::string& path) {
    std::ifstream file(path);
    std::string line, out;
    while (std::getline(file, line)) {
        if (line.rfind("//", 0) != 0) break;
        out += line + "\n";
    }
    return out;
}

} // namespace Json
