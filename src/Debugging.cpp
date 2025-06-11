#include "Debugging.h"

Debugging::Debugging() : VAO(0), VBO(0) {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);

    glBindVertexArray(VAO);

    float triVerts[] = {
        0,0,0,
        1,0,0,
        1,1,0,

        0,0,0,
        1,1,0,
        0,1,0,

        0,1,0,
        1,1,0,
        1,1,1,

        0,1,0,
        1,1,1,
        0,1,1,

        0,0,0,
        0,1,1,
        0,0,1,

        0,0,0,
        0,1,0,
        0,1,1,

        0,0,0,
        1,0,1,
        1,0,0,

        0,0,0,
        0,0,1,
        1,0,1,

        0,0,1,
        1,1,1,
        1,0,1,

        0,0,1,
        0,1,1,
        1,1,1,

        1,0,0,
        1,0,1,
        1,1,1,

        1,0,0,
        1,1,1,
        1,1,0,
    };
    
    float lineVerts[] = {
        0,0,0,
        1,0,0,

        1,0,0,
        1,1,0,

        1,1,0,
        0,1,0,

        0,1,0,
        0,0,0,

        0,0,1,
        1,0,1,

        1,0,1,
        1,1,1,

        1,1,1,
        0,1,1,

        0,1,1,
        0,0,1,

        0,0,0,
        0,0,1,

        0,1,0,
        0,1,1,

        1,0,0,
        1,0,1,

        1,1,0,
        1,1,1
    };

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(lineVerts), lineVerts, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
}

Debugging::~Debugging() {
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void Debugging::DrawCube(Shader& shader, glm::mat4 view, glm::mat4 projection, glm::vec3 pos)
{
    shader.Use();

    glm::mat4 model = glm::mat4(1.0f);
    shader.SetUniform("model", model);
    shader.SetUniform("projection", projection);

    glm::mat4 newViewMatrix = glm::translate(view, pos);
    shader.SetUniform("view", newViewMatrix);

    glDisable(GL_DEPTH_TEST);

    glBindVertexArray(VAO);
    glDrawArrays(GL_LINES, 0, 24);

    glEnable(GL_DEPTH_TEST);
}