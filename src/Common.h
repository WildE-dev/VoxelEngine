#pragma once
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#ifndef __gl_h_
#include <glad/glad.h>
#endif
#endif

#include <GLFW/glfw3.h>