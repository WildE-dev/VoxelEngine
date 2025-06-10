#pragma once
#include <glm/glm.hpp>
#include <glad/glad.h>
class Shader
{
public:
	Shader(const char* vertexPath, const char* fragmentPath);
	//Shader(const unsigned char* vertexSource, int vertexSize, const unsigned char* fragmentSource, int fragmentSize);
	~Shader();
	void Use();
	void SetUniform(const char* name, glm::mat4 matrix);
	void SetUniform(const char* name, float value);
	void SetUniform(const char* name, glm::vec2 value);
	void ReloadShader();
private:
	GLuint shaderProgram;
	const char *vertexPath, *fragmentPath;
};

