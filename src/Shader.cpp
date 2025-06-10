#include "Shader.h"
#include <iostream>
#include <fstream>
#include <string>
#include <glm/gtc/type_ptr.hpp>

GLuint CreateShaderFromFiles(const char* vertexPath, const char* fragmentPath);
GLuint CreateShaderFromStrings(const char* vertexShaderSource, const char* fragmentShaderSource);
GLuint CreateShaderFromResources(const unsigned char* vertexSource, int vertexSize, const unsigned char* fragmentSource, int fragmentSize);
GLuint CreateDefaultShader();

Shader::Shader(const char* vertexPath, const char* fragmentPath) {
    Shader::vertexPath = vertexPath;
    Shader::fragmentPath = fragmentPath;
    GLuint shader = CreateShaderFromFiles(vertexPath, fragmentPath);
    if (shader) {
        Shader::shaderProgram = shader;
    }
    else {
        Shader::shaderProgram = CreateDefaultShader();
    }
}

//Shader::Shader(const unsigned char* vertexSource, int vertexSize, const unsigned char* fragmentSource, int fragmentSize) {
//    GLuint shader = CreateShaderFromResources(vertexSource, vertexSize, fragmentSource, fragmentSize);
//    if (shader) {
//        Shader::shaderProgram = shader;
//    }
//    else {
//        Shader::shaderProgram = CreateDefaultShader();
//    }
//}

Shader::~Shader() {
    glDeleteProgram(Shader::shaderProgram);
}

void Shader::Use() {
    glUseProgram(Shader::shaderProgram);
}

void Shader::SetUniform(const char* name, glm::mat4 matrix) {
    unsigned int modelLoc = glGetUniformLocation(Shader::shaderProgram, name);
    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(matrix));
}

void Shader::SetUniform(const char* name, float value) {
    unsigned int modelLoc = glGetUniformLocation(Shader::shaderProgram, name);
    glUniform1f(modelLoc, value);
}

void Shader::SetUniform(const char* name, glm::vec2 value) {
    unsigned int modelLoc = glGetUniformLocation(Shader::shaderProgram, name);
    glUniform2fv(modelLoc, 1, glm::value_ptr(value));
}

void Shader::ReloadShader() {
    GLuint newShader = CreateShaderFromFiles(Shader::vertexPath, Shader::fragmentPath);
    glDeleteProgram(Shader::shaderProgram);
    if (newShader) {
        Shader::shaderProgram = newShader;
    }
    else {
        Shader::shaderProgram = CreateDefaultShader();
    }
}

GLuint CreateDefaultShader() {
    const char* vertexSource =
        "#version 330 core\n"
        "layout (location = 0) in vec3 aPos;"
        "uniform mat4 model;"
        "uniform mat4 view;"
        "uniform mat4 projection;"
        "void main()"
        "{"
        "   gl_Position = projection * view * model * vec4(aPos, 1.0f);"
        "}";
    const char* fragmentSource =
        "#version 330 core\n"
        "out vec4 FragColor;"
        "void main()"
        "{"
        "   FragColor = vec4(1.0, 0.0, 1.0, 1.0);"
        "}";

    return CreateShaderFromStrings(vertexSource, fragmentSource);
}

//GLuint CreateShaderFromResources(const unsigned char* vertexSource, int vertexSize, const unsigned char* fragmentSource, int fragmentSize) {
//    char vertex[vertexSize + 1];
//    memcpy(vertex, vertexSource, vertexSize);
//    vertex[vertexSize] = 0;
//
//    char fragment[fragmentSize + 1];
//    memcpy(fragment, fragmentSource, fragmentSize);
//    fragment[fragmentSize] = 0;
//    
//    return CreateShaderFromStrings(vertex, fragment);
//}

GLuint CreateShaderFromFiles(const char* vertexPath, const char* fragmentPath) {
    std::string line;
    std::ifstream file;

    file.open(vertexPath);

    if (!file.is_open()) {
        std::cerr << "Error opening the file: " << vertexPath << std::endl;
        return 0;
    }

    std::string vertexShaderSourceString;

    while (std::getline(file, line)) {
        vertexShaderSourceString += line + "\n";
    }

    file.close();

    file.open(fragmentPath);

    if (!file.is_open()) {
        std::cerr << "Error opening the file: " << fragmentPath << std::endl;
        return 0;
    }

    std::string fragmentShaderSourceString;

    while (std::getline(file, line)) {
        fragmentShaderSourceString += line + "\n";
    }

    file.close();

    const char* vertexShaderSource = vertexShaderSourceString.c_str();
    const char* fragmentShaderSource = fragmentShaderSourceString.c_str();

    return CreateShaderFromStrings(vertexShaderSource, fragmentShaderSource);
}

GLuint CreateShaderFromStrings(const char* vertexShaderSource, const char* fragmentShaderSource) {
    int success;
    char infoLog[512];

    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
    glCompileShader(vertexShader);

    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);

    if (!success)
    {
        glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
        std::cerr << "ERROR: Vertex shader could not be compiled" << std::endl << infoLog << std::endl;
        return 0;
    }

    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);

    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);

    if (!success)
    {
        glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
        std::cerr << "ERROR: Fragment shader could not be compiled" << std::endl << infoLog << std::endl;
        return 0;
    }

    GLuint shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, fragmentShader);
    glAttachShader(shaderProgram, vertexShader);
    glLinkProgram(shaderProgram);

    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);

    if (!success) {
        glGetProgramInfoLog(shaderProgram, 512, NULL, infoLog);
        std::cerr << "ERROR: Shader program could not be linked" << std::endl << infoLog << std::endl;
        return 0;
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return shaderProgram;
}