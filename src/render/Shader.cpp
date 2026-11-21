#include "render/Shader.h"

#include <fstream>
#include <iostream>
#include <sstream>

#include <glm/gtc/type_ptr.hpp>

namespace {

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open shader file: " << path << std::endl;
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    out = ss.str();
    return true;
}

GLuint CompileStage(GLenum type, const char* source, const char* label) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[1024];
        glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
        std::cerr << "ERROR: Could not compile " << label << std::endl << infoLog << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint CreateProgram(const char* vertexSource, const char* fragmentSource, const std::string& name) {
    GLuint vertex = CompileStage(GL_VERTEX_SHADER, vertexSource, (name + " (vertex)").c_str());
    if (!vertex) return 0;

    GLuint fragment = CompileStage(GL_FRAGMENT_SHADER, fragmentSource, (name + " (fragment)").c_str());
    if (!fragment) {
        glDeleteShader(vertex);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);

    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[1024];
        glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
        std::cerr << "ERROR: Could not link " << name << std::endl << infoLog << std::endl;
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

GLuint CreateProgramFromFiles(const std::string& vertexPath, const std::string& fragmentPath) {
    std::string vertexSource, fragmentSource;
    if (!ReadFile(vertexPath, vertexSource) || !ReadFile(fragmentPath, fragmentSource)) return 0;
    return CreateProgram(vertexSource.c_str(), fragmentSource.c_str(), vertexPath);
}

GLuint CreateFallbackProgram() {
    const char* vertexSource =
        "#version 330 core\n"
        "layout (location = 0) in vec3 aPos;\n"
        "uniform mat4 model;\n"
        "uniform mat4 view;\n"
        "uniform mat4 projection;\n"
        "void main() { gl_Position = projection * view * model * vec4(aPos, 1.0); }\n";
    const char* fragmentSource =
        "#version 330 core\n"
        "out vec4 FragColor;\n"
        "void main() { FragColor = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    return CreateProgram(vertexSource, fragmentSource, "fallback shader");
}

} // namespace

Shader::Shader(std::string vertexPath, std::string fragmentPath)
    : vertexPath(std::move(vertexPath)), fragmentPath(std::move(fragmentPath)) {
    program = CreateProgramFromFiles(this->vertexPath, this->fragmentPath);
    if (!program) program = CreateFallbackProgram();
}

Shader::~Shader() {
    glDeleteProgram(program);
}

void Shader::Use() const {
    glUseProgram(program);
}

void Shader::Reload() {
    GLuint newProgram = CreateProgramFromFiles(vertexPath, fragmentPath);
    if (!newProgram) {
        std::cerr << "Shader reload failed, keeping previous version: " << vertexPath << std::endl;
        return;
    }
    glDeleteProgram(program);
    program = newProgram;
    uniformCache.clear();
}

GLint Shader::GetLocation(const char* name) {
    auto it = uniformCache.find(name);
    if (it != uniformCache.end()) return it->second;

    GLint location = glGetUniformLocation(program, name);
    uniformCache.emplace(name, location);
    return location;
}

void Shader::SetUniform(const char* name, const glm::mat4& value) {
    glUniformMatrix4fv(GetLocation(name), 1, GL_FALSE, glm::value_ptr(value));
}

void Shader::SetUniform(const char* name, const glm::vec2& value) {
    glUniform2fv(GetLocation(name), 1, glm::value_ptr(value));
}

void Shader::SetUniform(const char* name, const glm::vec3& value) {
    glUniform3fv(GetLocation(name), 1, glm::value_ptr(value));
}

void Shader::SetUniform(const char* name, const glm::vec4& value) {
    glUniform4fv(GetLocation(name), 1, glm::value_ptr(value));
}

void Shader::SetUniform(const char* name, float value) {
    glUniform1f(GetLocation(name), value);
}

void Shader::SetUniform(const char* name, int value) {
    glUniform1i(GetLocation(name), value);
}
