#pragma once
#include <glm/glm.hpp>
#include <glad/glad.h>
class Shader
{
public:
	Shader(const char* vertexPath, const char* fragmentPath);
	~Shader();
	void Use();
	void SetUniform(const char* name, glm::mat4 matrix);
	void ReloadShader();
private:
	GLuint shaderProgram;
	const char *vertexPath, *fragmentPath;
};
