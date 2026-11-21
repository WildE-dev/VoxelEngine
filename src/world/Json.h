#pragma once
// A small JSON reader for the game's data files (Resources/Data). Parse() gives a
// Value tree, or an error with the line it was found on. Comments (// to the end of
// the line) are allowed, so the files can explain themselves.
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object; // (in file order)

    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    // A member of an object (nullptr if missing or not an object).
    const Value* Find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& [k, v] : object)
            if (k == key) return &v;
        return nullptr;
    }
    double NumberOr(const std::string& key, double fallback) const {
        const Value* v = Find(key);
        return v && v->IsNumber() ? v->number : fallback;
    }
    std::string StringOr(const std::string& key, const std::string& fallback) const {
        const Value* v = Find(key);
        return v && v->IsString() ? v->string : fallback;
    }
    bool BoolOr(const std::string& key, bool fallback) const {
        const Value* v = Find(key);
        return v && v->type == Type::Bool ? v->boolean : fallback;
    }
};

class Parser {
public:
    explicit Parser(std::string text) : s(std::move(text)) {}
    bool Parse(Value& out, std::string& error) {
        Skip();
        if (!ParseValue(out)) {
            error = message + " (line " + std::to_string(line) + ")";
            return false;
        }
        Skip();
        if (i != s.size()) {
            error = "unexpected text after the end (line " + std::to_string(line) + ")";
            return false;
        }
        return true;
    }

private:
    std::string s; // (its own copy)
    size_t i = 0;
    int line = 1;
    std::string message;

    bool Fail(const std::string& m) {
        message = m;
        return false;
    }
    void Skip() {
        while (i < s.size()) {
            if (s[i] == '\n') line++, i++;
            else if (std::isspace(static_cast<unsigned char>(s[i]))) i++;
            else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/')
                while (i < s.size() && s[i] != '\n') i++;
            else break;
        }
    }
    bool ParseValue(Value& v) {
        if (i >= s.size()) return Fail("unexpected end of file");
        const char c = s[i];
        if (c == '{') return ParseObject(v);
        if (c == '[') return ParseArray(v);
        if (c == '"') {
            v.type = Value::Type::String;
            return ParseString(v.string);
        }
        if (s.compare(i, 4, "true") == 0) return i += 4, v.type = Value::Type::Bool, v.boolean = true, true;
        if (s.compare(i, 5, "false") == 0) return i += 5, v.type = Value::Type::Bool, v.boolean = false, true;
        if (s.compare(i, 4, "null") == 0) return i += 4, v.type = Value::Type::Null, true;
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            char* end = nullptr;
            v.number = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) return Fail("bad number");
            i = static_cast<size_t>(end - s.c_str());
            v.type = Value::Type::Number;
            return true;
        }
        return Fail(std::string("unexpected '") + c + "'");
    }
    bool ParseString(std::string& out) {
        i++; // "
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\n') return Fail("a string runs past the end of the line");
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char e = s[++i];
                out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                i++;
            }
            else out += s[i++];
        }
        if (i >= s.size()) return Fail("a string isn't closed");
        i++;
        return true;
    }
    bool ParseArray(Value& v) {
        v.type = Value::Type::Array;
        i++;
        Skip();
        if (i < s.size() && s[i] == ']') return i++, true;
        for (;;) {
            Value item;
            Skip();
            if (!ParseValue(item)) return false;
            v.array.push_back(std::move(item));
            Skip();
            if (i < s.size() && s[i] == ',') {
                i++;
                Skip();
                if (i < s.size() && s[i] == ']') return i++, true; // (a trailing comma)
                continue;
            }
            if (i < s.size() && s[i] == ']') return i++, true;
            return Fail("expected ',' or ']' in a list");
        }
    }
    bool ParseObject(Value& v) {
        v.type = Value::Type::Object;
        i++;
        Skip();
        if (i < s.size() && s[i] == '}') return i++, true;
        for (;;) {
            Skip();
            if (i >= s.size() || s[i] != '"') return Fail("expected a \"name\"");
            std::string key;
            if (!ParseString(key)) return false;
            Skip();
            if (i >= s.size() || s[i] != ':') return Fail("expected ':' after \"" + key + "\"");
            i++;
            Skip();
            Value item;
            if (!ParseValue(item)) return false;
            v.object.emplace_back(std::move(key), std::move(item));
            Skip();
            if (i < s.size() && s[i] == ',') {
                i++;
                Skip();
                if (i < s.size() && s[i] == '}') return i++, true;
                continue;
            }
            if (i < s.size() && s[i] == '}') return i++, true;
            return Fail("expected ',' or '}' in an object");
        }
    }
};

// Reads and parses a file; false (with why, the file named) if it can't.
bool ParseFile(const std::string& path, Value& out, std::string& error);

// Writes a value as JSON for a person to read: anything that fits on a line of
// `width` goes on one, the rest is spread over lines, indented. Numbers are written as
// short as they'll go.
std::string Write(const Value& v, int width = 110);
// The comment lines at the top of a file (before its first '{'), to keep on saving.
std::string LeadingComments(const std::string& path);

// Setting up values (for the model editor).
inline Value Number(double n) { Value v; v.type = Value::Type::Number; v.number = n; return v; }
inline Value String(const std::string& s) { Value v; v.type = Value::Type::String; v.string = s; return v; }
inline Value Bool(bool b) { Value v; v.type = Value::Type::Bool; v.boolean = b; return v; }
inline Value Object() { Value v; v.type = Value::Type::Object; return v; }
inline Value Array() { Value v; v.type = Value::Type::Array; return v; }
// An object's member, made if missing.
inline Value& Member(Value& object, const std::string& key) {
    for (auto& [k, v] : object.object)
        if (k == key) return v;
    object.object.emplace_back(key, Value{});
    return object.object.back().second;
}
inline void Remove(Value& object, const std::string& key) {
    for (auto it = object.object.begin(); it != object.object.end(); ++it)
        if (it->first == key) {
            object.object.erase(it);
            return;
        }
}

} // namespace Json
