#pragma once

// Android-specific shader.h — uses GLES3 instead of GLEW/OpenGL
#include <GLES3/gl3.h>
#include <string>
#include <sstream>
#include <iostream>
#include <android/asset_manager.h>
#include <android/log.h>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "Planetary", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Planetary", __VA_ARGS__)

// Android asset manager — set from JNI before use
extern AAssetManager* g_assetManager;

class Shader {
public:
    GLuint id = 0;

    bool load(const std::string& vertPath, const std::string& fragPath) {
        std::string vertSrc = readAsset(vertPath);
        std::string fragSrc = readAsset(fragPath);
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
            LOGE("[Shader] Link error: %s", log);
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
    // Read shader source from Android assets
    std::string readAsset(const std::string& path) {
        if (!g_assetManager) {
            LOGE("[Shader] Asset manager not set!");
            return "";
        }
        AAsset* asset = AAssetManager_open(g_assetManager, path.c_str(), AASSET_MODE_BUFFER);
        if (!asset) {
            LOGE("[Shader] Cannot open asset: %s", path.c_str());
            return "";
        }
        size_t size = AAsset_getLength(asset);
        std::string content(size, '\0');
        AAsset_read(asset, &content[0], size);
        AAsset_close(asset);
        return content;
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
            LOGE("[Shader] Compile error: %s", log);
            return 0;
        }
        return s;
    }
};
