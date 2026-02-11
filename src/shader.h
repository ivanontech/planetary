#pragma once

#include <GL/glew.h>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>

class Shader {
public:
    GLuint id = 0;

    bool load(const std::string& vertPath, const std::string& fragPath) {
        std::string vertSrc = readFile(vertPath);
        std::string fragSrc = readFile(fragPath);
        if (vertSrc.empty() || fragSrc.empty()) return false;

        GLuint vert = compile(GL_VERTEX_SHADER, vertSrc);
        GLuint frag = compile(GL_FRAGMENT_SHADER, fragSrc);
        if (!vert || !frag) return false;

        id = glCreateProgram();
        glAttachShader(id, vert);
        glAttachShader(id, frag);
        glLinkProgram(id);

        GLint ok;
        glGetProgramiv(id, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(id, 512, nullptr, log);
            std::cerr << "[Shader] Link error: " << log << std::endl;
            return false;
        }

        glDeleteShader(vert);
        glDeleteShader(frag);
        return true;
    }

    void use() const { glUseProgram(id); }

    void setMat4(const char* name, const float* m) const {
        glUniformMatrix4fv(glGetUniformLocation(id, name), 1, GL_FALSE, m);
    }
    void setVec2(const char* name, float x, float y) const {
        glUniform2f(glGetUniformLocation(id, name), x, y);
    }
    void setVec3(const char* name, float x, float y, float z) const {
        glUniform3f(glGetUniformLocation(id, name), x, y, z);
    }
    void setVec4(const char* name, float x, float y, float z, float w) const {
        glUniform4f(glGetUniformLocation(id, name), x, y, z, w);
    }
    void setFloat(const char* name, float v) const {
        glUniform1f(glGetUniformLocation(id, name), v);
    }
    void setInt(const char* name, int v) const {
        glUniform1i(glGetUniformLocation(id, name), v);
    }

private:
    std::string readFile(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::cerr << "[Shader] Cannot open: " << path << std::endl;
            return "";
        }
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    GLuint compile(GLenum type, const std::string& src) {
        GLuint s = glCreateShader(type);
        const char* c = src.c_str();
        glShaderSource(s, 1, &c, nullptr);
        glCompileShader(s);
        GLint ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, 512, nullptr, log);
            std::cerr << "[Shader] Compile error: " << log << std::endl;
            return 0;
        }
        return s;
    }
};
