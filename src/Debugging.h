#pragma once

#include <glm/glm.hpp>
#include <glad/glad.h>
#include <glm/gtc/matrix_transform.hpp>

#include "Shader.h"

class Debugging
{
public:
    Debugging();
    ~Debugging();
    void DrawCube(Shader& shader, glm::mat4 view, glm::mat4 projection, glm::vec3 pos);

private:
    GLuint VAO, VBO;
};