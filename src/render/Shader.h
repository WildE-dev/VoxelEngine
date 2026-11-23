#pragma once

#include <string>
#include <unordered_map>

#include <glad/glad.h>
#include <glm/glm.hpp>

// A linked GLSL program loaded from a vertex + fragment file pair.
// Falls back to a magenta shader if the files fail to compile.
class Shader
{
public:
    Shader(std::string vertexPath, std::string fragmentPath);
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void Use() const;

    void SetUniform(const char* name, const glm::mat4& value);
    void SetUniform(const char* name, const glm::vec2& value);
    void SetUniform(const char* name, const glm::vec3& value);
    void SetUniform(const char* name, const glm::vec4& value);
    void SetUniform(const char* name, float value);
    void SetUniform(const char* name, int value);

    // For uniforms set many times a frame: look the location up once, then use gl calls.
    GLint GetLocation(const char* name);

    // Recompile from disk. Keeps the current program if the new one fails to build.
    void Reload();

private:

    GLuint program = 0;
    std::string vertexPath, fragmentPath;
    std::unordered_map<std::string, GLint> uniformCache;
};
