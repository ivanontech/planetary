// ============================================================
// PLANETARY - Native C++ / OpenGL (+ OpenGL ES for Android)
// Ported from the original Bloom Studio Planetary (Cinder/iOS)
// Music scanning from the Electron version
// Android TV / NVIDIA Shield port: streams from Navidrome (Subsonic API)
// ============================================================

#ifdef __ANDROID__
#include <GLES3/gl3.h>
#else
#include <GL/glew.h>
#endif
#include <SDL2/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

// Dear ImGui
#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl2.h"
#include "imgui/imgui_impl_opengl3.h"

#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <fstream>
#include <cstdlib>

#include "stb_image.h"
#include "shader.h"
#include "camera.h"
#include "music_data.h"
#include "miniaudio.h"

// Android-specific includes
#ifdef __ANDROID__
#include <android/log.h>
#define ANDROID_LOG(msg) __android_log_print(ANDROID_LOG_DEBUG, "Planetary", "%s", (msg).c_str())
#else
#define ANDROID_LOG(msg)
#endif

// ============================================================
// FORCE DISCRETE GPU (NVIDIA / AMD)
// This tells the driver to use the RTX 5090 instead of integrated
// ============================================================
#ifdef _WIN32
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// ============================================================
// GLOBALS - from original Globals.h
// ============================================================
static const int G_ALPHA_LEVEL  = 1;
static const int G_ARTIST_LEVEL = 2;
static const int G_ALBUM_LEVEL  = 3;
static const int G_TRACK_LEVEL  = 4;

static const glm::vec3 BRIGHT_BLUE{0.4f, 0.8f, 1.0f};
static const glm::vec3 BLUE{0.1f, 0.2f, 0.5f};
static const glm::vec3 GREY{0.1f, 0.1f, 0.15f};

// ============================================================
// RESOURCE PATH RESOLUTION
// On Windows, double-clicking the exe sets CWD to something random.
// We resolve all paths relative to the exe's actual directory.
// ============================================================
static std::string g_basePath;

std::string resolvePath(const std::string& relative) {
    return g_basePath + relative;
}

void initBasePath() {
    char* sdlBase = SDL_GetBasePath();
    if (sdlBase) {
        g_basePath = sdlBase;
        SDL_free(sdlBase);
    } else {
        g_basePath = "./";
    }
    std::cout << "[Planetary] Base path: " << g_basePath << std::endl;
}

// ============================================================
// NODE STRUCTURES - from original Node.h, NodeArtist, etc.
// ============================================================

struct ArtistNode {
    int index;
    std::string name;
    glm::vec3 pos;
    float hue, sat;
    glm::vec3 color;
    glm::vec3 glowColor;
    float radiusInit;
    float radius;
    float glowRadius;
    float idealCameraDist;
    int totalTracks;
    bool isSelected = false;

    struct AlbumOrbit {
        float radius;
        float angle;
        float speed;
        float planetSize;
        int numTracks;
        std::string name;
        int artistIndex; // back-reference
        int albumIndex;
        struct TrackOrbit {
            float radius;
            float angle;
            float speed;
            float size;
            std::string name;
            std::string filePath;
            float duration;
            float tiltX, tiltZ;  // Unique orbital plane tilt
        };
        std::vector<TrackOrbit> tracks;
    };
    std::vector<AlbumOrbit> albumOrbits;
};

// ============================================================
// ARTIST COLOR - exact port from NodeArtist.cpp
// ============================================================
void computeArtistColor(ArtistNode& node) {
    const std::string& name = node.name;
    char c1 = ' ', c2 = ' ';
    if (name.length() >= 3) { c1 = name[1]; c2 = name[2]; }
    int c1Int = std::clamp((int)c1, 32, 127);
    int c2Int = std::clamp((int)c2, 32, 127);
    int totalCharAscii = (c1Int - 32) + (c2Int - 32);
    float asciiPer = ((float)totalCharAscii / 190.0f) * 5000.0f;

    node.hue = sinf(asciiPer) * 0.35f + 0.35f;
    node.sat = (1.0f - sinf((node.hue + 0.15f) * (float)M_PI)) * 0.75f;

    auto hsvToRgb = [](float h, float s, float v) -> glm::vec3 {
        float c = v * s;
        float x = c * (1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
        float m = v - c;
        glm::vec3 rgb;
        int hi = (int)(h * 6.0f) % 6;
        switch (hi) {
            case 0: rgb = {c,x,0}; break; case 1: rgb = {x,c,0}; break;
            case 2: rgb = {0,c,x}; break; case 3: rgb = {0,x,c}; break;
            case 4: rgb = {x,0,c}; break; default: rgb = {c,0,x}; break;
        }
        return rgb + glm::vec3(m);
    };

    node.color = hsvToRgb(node.hue, std::max(node.sat, 0.5f), 1.0f);
    node.glowColor = hsvToRgb(node.hue, std::min(node.sat + 0.2f, 1.0f), 1.0f);
    node.radiusInit = 1.25f + (0.66f - node.hue);
    node.radius = node.radiusInit;
}

// ============================================================
// STAR POSITIONING - from NodeArtist.cpp setData()
// ============================================================
// Genre -> angular sector mapping for spatial clustering
static std::map<std::string, float> g_genreAngles;
static float g_nextGenreAngle = 0;

float getGenreAngle(const std::string& genre) {
    auto it = g_genreAngles.find(genre);
    if (it != g_genreAngles.end()) return it->second;
    float a = g_nextGenreAngle;
    g_nextGenreAngle += 0.618f * 2.0f * M_PI; // golden angle separation
    g_genreAngles[genre] = a;
    return a;
}

void computeArtistPosition(ArtistNode& node, int total, const std::string& genre) {
    std::hash<std::string> hasher;
    size_t h = hasher(node.name);
    float hashPer = (float)(h % 9000L) / 90.0f + 10.0f;
    float spreadFactor = 3.0f;
    hashPer *= spreadFactor;

    // Base angle from genre cluster + offset from name hash
    float genreBase = getGenreAngle(genre);
    float nameOffset = (float)(h % 628) / 100.0f; // 0 to ~6.28 (full circle)
    float genreSpread = 0.8f; // How tight the cluster is (radians)
    float angle = genreBase + nameOffset * genreSpread;

    // Vertical from second hash
    size_t h2 = hasher(node.name + "_y");
    float yHash = ((float)(h2 % 10000) / 10000.0f - 0.5f) * 2.0f;
    float height = yHash * hashPer * 0.35f;

    node.pos = glm::vec3(cosf(angle) * hashPer, height, sinf(angle) * hashPer);
}

// ============================================================
// ALBUM ORBIT LAYOUT - from NodeArtist::setChildOrbitRadii()
// ============================================================
void computeAlbumOrbits(ArtistNode& node, const ArtistData& artistData, int artistIdx) {
    node.albumOrbits.clear();
    float orbitOffset = node.radiusInit * 1.25f;
    int albumIdx = 0;
    for (auto& album : artistData.albums) {
        ArtistNode::AlbumOrbit orbit;
        orbit.name = album.name;
        orbit.numTracks = (int)album.tracks.size();
        orbit.artistIndex = artistIdx;
        orbit.albumIndex = albumIdx;
        float amt = std::max(orbit.numTracks * 0.065f, 0.2f);
        orbitOffset += amt;
        orbit.radius = orbitOffset;
        orbit.angle = (float)albumIdx * 0.618f * (float)M_PI * 2.0f;
        orbit.speed = 0.025f / sqrtf(std::max(orbit.radius, 0.5f)); // Slow, majestic planetary orbits
        orbit.planetSize = std::max(0.15f, 0.1f + sqrtf((float)orbit.numTracks) * 0.06f);

        float trackOrbitR = orbit.planetSize * 3.0f;
        for (int ti = 0; ti < (int)album.tracks.size(); ti++) {
            ArtistNode::AlbumOrbit::TrackOrbit to;
            to.name = album.tracks[ti].title;
            to.filePath = album.tracks[ti].filePath;
            to.duration = album.tracks[ti].duration;
            float moonSize = std::max(0.04f, 0.02f + 0.03f * (to.duration / 300.0f));
            trackOrbitR += moonSize * 2.0f;
            to.radius = trackOrbitR;
            to.angle = (float)ti * 2.396f;
            // Orbit speed: gentle orbital motion (slower = easier to click)
            // 3-min track orbits in ~60s, 5-min in ~100s
            to.speed = (2.0f * (float)M_PI) / (std::max(to.duration, 60.0f) * 0.35f);
            to.size = moonSize;
            // Unique orbital tilt per moon -- 3D orbits not flat
            std::hash<std::string> th;
            size_t trackHash = th(to.name + std::to_string(ti));
            to.tiltX = ((float)(trackHash % 1000) / 1000.0f - 0.5f) * 0.5f;
            to.tiltZ = ((float)((trackHash >> 10) % 1000) / 1000.0f - 0.5f) * 0.4f;
            trackOrbitR += moonSize * 2.0f;
            orbit.tracks.push_back(to);
        }
        orbitOffset += amt;
        albumIdx++;
        node.albumOrbits.push_back(orbit);
    }
    node.idealCameraDist = std::max(orbitOffset * 2.6f, 8.0f);
}

// ============================================================
// TEXTURE LOADING
// ============================================================
GLuint loadTexture(const std::string& path) {
    std::string fullPath = resolvePath(path);
    int w, h, channels;
    unsigned char* data = stbi_load(fullPath.c_str(), &w, &h, &channels, 4);
    if (!data) {
        std::cerr << "[Planetary] Failed to load texture: " << fullPath << std::endl;
        return 0;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    stbi_image_free(data);
    std::cout << "[Planetary] Loaded: " << path << " (" << w << "x" << h << ")" << std::endl;
    return tex;
}

// ============================================================
// SPHERE MESH
// ============================================================
struct SphereMesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
    int indexCount = 0;
    void create(int stacks, int slices) {
        std::vector<float> verts;
        std::vector<unsigned int> indices;
        for (int i = 0; i <= stacks; i++) {
            float phi = (float)M_PI * (float)i / stacks;
            for (int j = 0; j <= slices; j++) {
                float theta = 2.0f * (float)M_PI * (float)j / slices;
                float x = sinf(phi) * cosf(theta), y = cosf(phi), z = sinf(phi) * sinf(theta);
                verts.push_back(x); verts.push_back(y); verts.push_back(z);
                verts.push_back(x); verts.push_back(y); verts.push_back(z);
                verts.push_back((float)j / slices); verts.push_back((float)i / stacks);
            }
        }
        for (int i = 0; i < stacks; i++)
            for (int j = 0; j < slices; j++) {
                int a = i * (slices + 1) + j, b = a + slices + 1;
                indices.push_back(a); indices.push_back(b); indices.push_back(a + 1);
                indices.push_back(b); indices.push_back(b + 1); indices.push_back(a + 1);
            }
        indexCount = (int)indices.size();
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); glGenBuffers(1, &ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3*sizeof(float))); glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6*sizeof(float))); glEnableVertexAttribArray(2);
        glBindVertexArray(0);
    }
    void draw() const { glBindVertexArray(vao); glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0); glBindVertexArray(0); }
};

// ============================================================
// RING MESH
// ============================================================
struct RingMesh {
    GLuint vao = 0, vbo = 0; int vertCount = 0;
    void create(float radius, int segments) {
        std::vector<float> verts;
        for (int i = 0; i <= segments; i++) {
            float a = 2.0f * (float)M_PI * (float)i / segments;
            verts.push_back(cosf(a) * radius); verts.push_back(0); verts.push_back(sinf(a) * radius);
        }
        vertCount = segments + 1;
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
        glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), 0); glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }
    void draw() const { glBindVertexArray(vao); glDrawArrays(GL_LINE_STRIP, 0, vertCount); glBindVertexArray(0); }
};

// ============================================================
// SATURN RING DISC MESH (annulus for planet rings)
// ============================================================
struct RingDiscMesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
    int indexCount = 0;
    void create(float innerR, float outerR, int segments) {
        std::vector<float> verts;
        std::vector<unsigned int> indices;
        for (int i = 0; i <= segments; i++) {
            float angle = 2.0f * (float)M_PI * (float)i / segments;
            float ca = cosf(angle), sa = sinf(angle);
            // Inner vertex: pos(3) + texcoord(2)
            verts.push_back(ca * innerR); verts.push_back(0); verts.push_back(sa * innerR);
            verts.push_back(0.0f); verts.push_back((float)i / segments);
            // Outer vertex
            verts.push_back(ca * outerR); verts.push_back(0); verts.push_back(sa * outerR);
            verts.push_back(1.0f); verts.push_back((float)i / segments);
        }
        for (int i = 0; i < segments; i++) {
            int a = i * 2, b = a + 1, c = a + 2, d = a + 3;
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
        indexCount = (int)indices.size();
        glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo); glGenBuffers(1, &ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
    }
    void draw() const { glBindVertexArray(vao); glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0); glBindVertexArray(0); }
};

// ============================================================
// BACKGROUND STARS
// ============================================================
struct BackgroundStars {
    GLuint vao = 0, vbo = 0; int count = 0;
    void create(int n) {
        count = n; std::vector<float> data;
        std::mt19937 rng(42); std::uniform_real_distribution<float> d(-1,1), b(0.1f,0.8f);
        for (int i = 0; i < n; i++) {
            float x=d(rng),y=d(rng),z=d(rng); float len=sqrtf(x*x+y*y+z*z);
            if(len<0.001f)continue; float r=300+d(rng)*200; x=x/len*r;y=y/len*r;z=z/len*r;
            float br=b(rng);
            data.push_back(x);data.push_back(y);data.push_back(z);
            data.push_back(br*0.8f);data.push_back(br*0.85f);data.push_back(br);data.push_back(br*0.6f);
            data.push_back(0.5f+d(rng)*0.5f);
        }
        glGenVertexArrays(1,&vao);glGenBuffers(1,&vbo);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);
        glBufferData(GL_ARRAY_BUFFER,data.size()*sizeof(float),data.data(),GL_STATIC_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,8*sizeof(float),0);glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,1,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(7*sizeof(float)));glEnableVertexAttribArray(2);
        glBindVertexArray(0);
    }
    void draw() const { glBindVertexArray(vao); glDrawArrays(GL_POINTS, 0, count); glBindVertexArray(0); }
};

// ============================================================
// BILLBOARD QUAD
// ============================================================
struct BillboardQuad {
    GLuint vao=0, vbo=0;
    void create() {
        float v[]={0,0,0,0,0,1,1,1,1,1, 0,0,0,1,0,1,1,1,1,1, 0,0,0,1,1,1,1,1,1,1,
                   0,0,0,0,0,1,1,1,1,1, 0,0,0,1,1,1,1,1,1,1, 0,0,0,0,1,1,1,1,1,1};
        glGenVertexArrays(1,&vao);glGenBuffers(1,&vbo);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);
        glBufferData(GL_ARRAY_BUFFER,sizeof(v),v,GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,10*sizeof(float),0);glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,10*sizeof(float),(void*)(3*sizeof(float)));glEnableVertexAttribArray(1);
        glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,10*sizeof(float),(void*)(5*sizeof(float)));glEnableVertexAttribArray(2);
        glVertexAttribPointer(3,1,GL_FLOAT,GL_FALSE,10*sizeof(float),(void*)(9*sizeof(float)));glEnableVertexAttribArray(3);
        glBindVertexArray(0);
    }
    void draw(glm::vec3 p, glm::vec4 c, float s) {
        float v[]={p.x,p.y,p.z,0,0,c.r,c.g,c.b,c.a,s, p.x,p.y,p.z,1,0,c.r,c.g,c.b,c.a,s,
                   p.x,p.y,p.z,1,1,c.r,c.g,c.b,c.a,s, p.x,p.y,p.z,0,0,c.r,c.g,c.b,c.a,s,
                   p.x,p.y,p.z,1,1,c.r,c.g,c.b,c.a,s, p.x,p.y,p.z,0,1,c.r,c.g,c.b,c.a,s};
        glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,vbo);
        glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(v),v);glDrawArrays(GL_TRIANGLES,0,6);glBindVertexArray(0);
    }
};

// ============================================================
// AUDIO PLAYER - miniaudio (supports MP3, FLAC, WAV, OGG, etc.)
// ============================================================
struct AudioPlayer {
    ma_engine engine;
    ma_sound sound;
    bool engineInit = false;
    bool soundInit = false;
    bool playing = false;
    float volume = 0.8f;
    std::string currentTrack;
    std::string currentTrackName;
    std::string currentArtist;
    std::string currentAlbum;
    float duration = 0;
    bool castEnabled = false;
    std::string castTarget = "Living Room";

    static std::string shellEscapeSingleQuotes(const std::string& in) {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        return out;
    }

    void init() {
        ma_engine_config config = ma_engine_config_init();
        if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
            std::cerr << "[Audio] Failed to init miniaudio engine" << std::endl;
            return;
        }
        engineInit = true;
        ma_engine_set_volume(&engine, volume);
        const char* castEnv = std::getenv("PLANETARY_CAST");
        const char* targetEnv = std::getenv("PLANETARY_CAST_TARGET");
        castEnabled = (castEnv && std::string(castEnv) == "1");
        if (targetEnv && std::string(targetEnv).size() > 0) castTarget = targetEnv;
        std::cout << "[Audio] miniaudio engine ready (MP3/FLAC/WAV/OGG)";
        if (castEnabled) std::cout << " | CAST MODE ON -> " << castTarget;
        std::cout << std::endl;
    }

    void play(const std::string& path, const std::string& name, const std::string& artist, const std::string& album, float dur) {
        if (!engineInit) return;

        // Stop current track
        if (soundInit) {
            ma_sound_uninit(&sound);
            soundInit = false;
        }

        currentTrack = path;
        currentTrackName = name;
        currentArtist = artist;
        currentAlbum = album;
        duration = dur;

        if (castEnabled) {
            std::string escapedPath = shellEscapeSingleQuotes(path);
            std::string escapedTarget = shellEscapeSingleQuotes(castTarget);
            std::string cmd = "/Users/kawkaw/.openclaw/workspace/.venv_music/bin/python /Users/kawkaw/.openclaw/workspace/scripts/planetary_cast_track.py --file '" + escapedPath + "' --target '" + escapedTarget + "' >/tmp/planetary_cast_track.log 2>&1 &";
            int rc = std::system(cmd.c_str());
            playing = (rc == 0);
            std::cout << "[Audio] Cast: " << name << " by " << artist << " -> " << castTarget << std::endl;
            return;
        }

#ifdef __ANDROID__
        // Android: HTTP URLs from Navidrome need to be streamed
        // Use SDL_RWops or download to temp file
        // For simplicity: download to /data/local/tmp/planetary_current.mp3
        if (path.substr(0, 4) == "http") {
            std::string tempFile = "/data/local/tmp/planetary_stream.mp3";
            // Download in background then play
            // For now: use planetaryHttpGet from music_data.h to buffer the first 5MB
            // and save to temp file, then init from file
            // This is a blocking prefetch — runs in background thread
            std::string response = planetaryHttpGet(path, 30);
            if (response.empty()) {
                __android_log_print(ANDROID_LOG_ERROR, "Planetary", "[Audio] HTTP stream failed: %s", path.c_str());
                return;
            }
            // Write to temp file
            FILE* f = fopen(tempFile.c_str(), "wb");
            if (!f) {
                __android_log_print(ANDROID_LOG_ERROR, "Planetary", "[Audio] Cannot write temp file");
                return;
            }
            fwrite(response.data(), 1, response.size(), f);
            fclose(f);
            if (ma_sound_init_from_file(&engine, tempFile.c_str(), 0, nullptr, nullptr, &sound) != MA_SUCCESS) {
                __android_log_print(ANDROID_LOG_ERROR, "Planetary", "[Audio] Failed to decode stream");
                return;
            }
        } else {
            if (ma_sound_init_from_file(&engine, path.c_str(), 0, nullptr, nullptr, &sound) != MA_SUCCESS) {
                __android_log_print(ANDROID_LOG_ERROR, "Planetary", "[Audio] Failed to load: %s", path.c_str());
                return;
            }
        }
#else
        if (ma_sound_init_from_file(&engine, path.c_str(), 0, nullptr, nullptr, &sound) != MA_SUCCESS) {
            std::cerr << "[Audio] Failed to load: " << path << std::endl;
            return;
        }
#endif
        soundInit = true;
        ma_sound_start(&sound);
        playing = true;
        std::cout << "[Audio] Playing: " << name << " by " << artist << std::endl;
    }

    void togglePause() {
        if (!soundInit) return;
        if (playing) {
            ma_sound_stop(&sound);
            playing = false;
        } else {
            ma_sound_start(&sound);
            playing = true;
        }
    }

    void stop() {
        if (soundInit) {
            ma_sound_stop(&sound);
            playing = false;
        }
    }

    float progress() const {
        if (!soundInit || duration <= 0) return 0;
        float cursor = 0;
        ma_sound_get_cursor_in_seconds(&sound, &cursor);
        return cursor / duration;
    }

    float currentTime() const {
        if (!soundInit) return 0;
        float cursor = 0;
        ma_sound_get_cursor_in_seconds(&sound, &cursor);
        return cursor;
    }

    void setVolume(float v) {
        volume = v;
        if (engineInit) ma_engine_set_volume(&engine, v);
    }

    bool isAtEnd() const {
        if (!soundInit) return false;
        return ma_sound_at_end(&sound);
    }

    void cleanup() {
        if (soundInit) ma_sound_uninit(&sound);
        if (engineInit) ma_engine_uninit(&engine);
    }
};

// ============================================================
// APPLICATION STATE
// ============================================================
struct App {
    SDL_Window* window = nullptr;
    SDL_GLContext glContext = nullptr;
    int screenW = 1400, screenH = 900;
    bool running = true;

    Camera camera;
    MusicLibrary library;
    std::vector<ArtistNode> artistNodes;
    int currentLevel = G_ALPHA_LEVEL;
    int selectedArtist = -1;
    int selectedAlbum = -1;
    char searchBuf[256] = {0};  // Search buffer -- directly used by ImGui InputText

    // Rendering
    Shader starPointShader, billboardShader, planetShader, ringShader;
    Shader bloomBrightShader, bloomBlurShader, bloomCompositeShader;
    Shader starSurfaceShader, saturnRingShader, gravityRippleShader;
    GLuint texStarGlow=0, texAtmosphere=0, texStar=0, texSurface=0, texSkydome=0;
    GLuint texLensFlare=0, texStarCore=0, texEclipseGlow=0, texParticle=0;
    GLuint texPlanetClouds[5] = {0};
    RingDiscMesh ringDisc;
    BackgroundStars bgStars;
    BillboardQuad billboard;
    SphereMesh sphereHi, sphereMd, sphereLo;
    RingMesh unitRing;

    // Bloom framebuffers
    GLuint sceneFBO=0, sceneColor=0, sceneDepth=0;
    GLuint bloomFBO[2]={0,0}, bloomColor[2]={0,0};
    GLuint quadVAO=0, quadVBO=0;
    int bloomW=0, bloomH=0;

    // Audio
    AudioPlayer audio;

    // Album art textures (artistIdx_albumIdx -> GL texture)
    std::map<std::string, GLuint> albumArtTextures;

    float elapsedTime = 0;
    bool mouseDown = false;
    int mouseButton = 0;
    bool imguiWantsMouse = false;
    int mouseDragDist = 0;  // Accumulated drag pixels to distinguish click vs drag
    int mouseDownX = 0, mouseDownY = 0;

    // Multi-size fonts for text labels
    ImFont* fontLarge = nullptr;   // 28px for artist names
    ImFont* fontMedium = nullptr;  // 20px for album names
    ImFont* fontSmall = nullptr;   // 13px for track names

    // Virtual keyboard for controller search
    bool showVirtualKB = false;
    int vkbRow = 1, vkbCol = 0;   // Cursor position in keyboard grid
    std::string vkbInput;          // Text being typed
    float vkbRepeatTimer = 0;     // For D-pad repeat delay

    // Currently playing track location (for trail rendering)
    int playingArtist = -1, playingAlbum = -1, playingTrack = -1;

    // Shooting star meteors (random tracks)
    struct Meteor {
        glm::vec3 pos, vel;
        glm::vec3 color;
        float life, maxLife;
        float size;
        std::string trackName;
        std::vector<glm::vec3> trail;
    };
    std::vector<Meteor> meteors;
    float nextMeteorTime = 3.0f;

    // Comet particles (long glowing tails from galaxy edges)
    struct Comet {
        glm::vec3 pos, vel, accel;
        glm::vec3 color;
        float life, maxLife;
        float headSize;
        std::vector<glm::vec3> tail;
    };
    std::vector<Comet> comets;
    float nextCometTime = 10.0f;

    // Persistent config
    std::string configPath;

    // Audio analysis (for reactive visuals)
    float audioLevel = 0;       // Current RMS volume 0-1
    float audioPeak = 0;        // Peak detector
    float audioBass = 0;        // Low-freq energy
    float audioWave = 0;        // Smooth wave for pulsation

    // Gamepad
    SDL_GameController* controller = nullptr;

    // Loading state
    std::atomic<bool> libraryLoaded{false};
    std::atomic<bool> scanning{false};
    std::atomic<int> scanProgress{0};
    std::atomic<int> scanTotal{0};
    std::string musicPath;
    std::string statusMsg;
};

// ============================================================
// PERSISTENT CONFIG - save/load music library path
// ============================================================
void saveConfig(App& app) {
    std::string path = g_basePath + "planetary.cfg";
    std::ofstream f(path);
    if (f.is_open()) {
        f << app.musicPath << std::endl;
        std::cout << "[Config] Saved: " << path << std::endl;
    }
}

std::string loadConfig() {
    std::string path = g_basePath + "planetary.cfg";
    std::ifstream f(path);
    if (f.is_open()) {
        std::string musicPath;
        std::getline(f, musicPath);
#ifdef __ANDROID__
        // On Android, always use the Navidrome server URL
        if (!musicPath.empty()) {
            std::cout << "[Config] Loaded: " << musicPath << std::endl;
            return musicPath;
        }
#else
        if (!musicPath.empty() && fs::is_directory(musicPath)) {
            std::cout << "[Config] Loaded: " << musicPath << std::endl;
            return musicPath;
        }
#endif
    }
    return "";
}

// ============================================================
// AUDIO ANALYSIS - extract volume/frequency for reactive visuals
// ============================================================
void updateAudioAnalysis(App& app, float dt) {
    if (!app.audio.soundInit || !app.audio.playing) {
        app.audioLevel *= 0.95f; // Decay
        app.audioPeak *= 0.98f;
        app.audioBass *= 0.95f;
        app.audioWave *= 0.97f;
        return;
    }

    // Get current cursor position as a proxy for beat detection
    float cursor = app.audio.currentTime();
    float progress = app.audio.progress();

    // Simulate audio energy using time-based patterns
    // (Real FFT would need a custom audio callback -- this approximation works visually)
    float t = cursor * 8.0f; // ~8 "beats" per second at 120bpm
    float beat = powf(fabsf(sinf(t * (float)M_PI)), 4.0f); // Sharp peaks
    float bass = powf(fabsf(sinf(t * (float)M_PI * 0.5f)), 2.0f); // Slower bass

    app.audioLevel = 0.3f + beat * 0.7f;
    app.audioPeak = std::max(app.audioPeak * 0.97f, app.audioLevel);
    app.audioBass = 0.2f + bass * 0.8f;
    app.audioWave += (app.audioLevel - app.audioWave) * 5.0f * dt;
}

// ============================================================
// INIT
// ============================================================
bool initSDL(App& app) {
    // Steam Input compatibility: enable controller events even when backgrounded
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Enable Steam virtual gamepad (Steam Input translates all controllers to XInput)
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    // Enable PS4/PS5 controller support via SDL
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return false;
    }
    initBasePath();

#ifdef __ANDROID__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    app.window = SDL_CreateWindow("Planetary",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        app.screenW, app.screenH,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN | SDL_WINDOW_MAXIMIZED);
    if (!app.window) { std::cerr << "Window failed: " << SDL_GetError() << std::endl; return false; }

    app.glContext = SDL_GL_CreateContext(app.window);
    if (!app.glContext) { std::cerr << "GL context failed: " << SDL_GetError() << std::endl; return false; }
    SDL_GL_SetSwapInterval(1);

    // Get actual window size after maximize
    SDL_GetWindowSize(app.window, &app.screenW, &app.screenH);
    app.camera.aspect = (float)app.screenW / (float)app.screenH;

#ifndef __ANDROID__
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { std::cerr << "GLEW init failed" << std::endl; return false; }
#endif

    std::cout << "[Planetary] OpenGL " << glGetString(GL_VERSION) << std::endl;
    std::cout << "[Planetary] GPU: " << glGetString(GL_RENDERER) << std::endl;

    // Init Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Load Montserrat Bold at multiple sizes for text hierarchy
    std::string fontPath = resolvePath("resources/Montserrat-Bold.ttf");
    ImFont* defaultFont = io.Fonts->AddFontDefault();                          // Fonts[0]
    ImFont* boldFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f);  // Fonts[1] - UI
    app.fontLarge  = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 28.0f);    // Fonts[2] - artist names
    app.fontMedium = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 20.0f);    // Fonts[3] - album names
    app.fontSmall  = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 13.0f);    // Fonts[4] - track names
    if (!boldFont) {
        std::cerr << "[Planetary] Failed to load font, using default" << std::endl;
        ImFontConfig cfg; cfg.SizePixels = 16.0f;
        boldFont = io.Fonts->AddFontDefault(&cfg);
        app.fontLarge = boldFont;
        app.fontMedium = boldFont;
        app.fontSmall = boldFont;
    }
    ImGui::StyleColorsDark();

    // Style - dark space theme
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(12, 12);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.02f, 0.85f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.0f, 0.05f, 0.1f, 0.9f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.15f, 0.25f, 0.9f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.1f, 0.25f, 0.4f, 0.8f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.15f, 0.35f, 0.55f, 0.9f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.4f, 0.8f, 1.0f, 0.8f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.05f, 0.1f, 0.15f, 0.8f);
    style.Colors[ImGuiCol_Text] = ImVec4(0.8f, 0.9f, 1.0f, 1.0f);

    ImGui_ImplSDL2_InitForOpenGL(app.window, app.glContext);
#ifdef __ANDROID__
    ImGui_ImplOpenGL3_Init("#version 300 es");
#else
    ImGui_ImplOpenGL3_Init("#version 330");
#endif

    // Init audio
    app.audio.init();

    // Open first available gamepad (PS5/Xbox controller via SDL2)
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            app.controller = SDL_GameControllerOpen(i);
            if (app.controller) {
                std::cout << "[Controller] " << SDL_GameControllerName(app.controller) << std::endl;
                break;
            }
        }
    }

    return true;
}

void setupBloom(App& app) {
    app.bloomW = app.screenW / 2;
    app.bloomH = app.screenH / 2;

    // Scene FBO (full res, HDR-ish)
    if (app.sceneFBO) { glDeleteFramebuffers(1, &app.sceneFBO); glDeleteTextures(1, &app.sceneColor); glDeleteRenderbuffers(1, &app.sceneDepth); }
    glGenFramebuffers(1, &app.sceneFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, app.sceneFBO);
    glGenTextures(1, &app.sceneColor);
    glBindTexture(GL_TEXTURE_2D, app.sceneColor);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, app.screenW, app.screenH, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, app.sceneColor, 0);
    glGenRenderbuffers(1, &app.sceneDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, app.sceneDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, app.screenW, app.screenH);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, app.sceneDepth);

    // Bloom FBOs (half res, ping-pong)
    for (int i = 0; i < 2; i++) {
        if (app.bloomFBO[i]) { glDeleteFramebuffers(1, &app.bloomFBO[i]); glDeleteTextures(1, &app.bloomColor[i]); }
        glGenFramebuffers(1, &app.bloomFBO[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, app.bloomFBO[i]);
        glGenTextures(1, &app.bloomColor[i]);
        glBindTexture(GL_TEXTURE_2D, app.bloomColor[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, app.bloomW, app.bloomH, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, app.bloomColor[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Fullscreen quad
    if (!app.quadVAO) {
        float qv[] = { -1,-1,0,0, 1,-1,1,0, 1,1,1,1, -1,-1,0,0, 1,1,1,1, -1,1,0,1 };
        glGenVertexArrays(1, &app.quadVAO); glGenBuffers(1, &app.quadVBO);
        glBindVertexArray(app.quadVAO); glBindBuffer(GL_ARRAY_BUFFER, app.quadVBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(qv), qv, GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), 0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4*sizeof(float), (void*)(2*sizeof(float))); glEnableVertexAttribArray(1);
        glBindVertexArray(0);
    }
}

void drawFullscreenQuad(App& app) {
    glBindVertexArray(app.quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

bool initResources(App& app) {
#ifdef __ANDROID__
    const std::string shaderDir = "shaders/es/";
#else
    const std::string shaderDir = "shaders/";
#endif
    if (!app.starPointShader.load(resolvePath(shaderDir+"star_points.vert"), resolvePath(shaderDir+"star_points.frag"))) return false;
    if (!app.billboardShader.load(resolvePath(shaderDir+"billboard.vert"), resolvePath(shaderDir+"billboard.frag"))) return false;
    if (!app.planetShader.load(resolvePath(shaderDir+"planet.vert"), resolvePath(shaderDir+"planet.frag"))) return false;
    if (!app.ringShader.load(resolvePath(shaderDir+"orbit_ring.vert"), resolvePath(shaderDir+"orbit_ring.frag"))) return false;
    if (!app.bloomBrightShader.load(resolvePath(shaderDir+"fullscreen.vert"), resolvePath(shaderDir+"bloom_bright.frag"))) return false;
    if (!app.bloomBlurShader.load(resolvePath(shaderDir+"fullscreen.vert"), resolvePath(shaderDir+"bloom_blur.frag"))) return false;
    if (!app.bloomCompositeShader.load(resolvePath(shaderDir+"fullscreen.vert"), resolvePath(shaderDir+"bloom_composite.frag"))) return false;

    // Procedural fire star shader (vertex displacement + turbulent fire)
    app.starSurfaceShader.load(resolvePath(shaderDir+"star.vert"), resolvePath(shaderDir+"star.frag"));
    // Saturn-like ring shader for large albums
    app.saturnRingShader.load(resolvePath(shaderDir+"saturn_ring.vert"), resolvePath(shaderDir+"saturn_ring.frag"));
    // Gravity ripple post-process (bass-reactive space distortion)
    app.gravityRippleShader.load(resolvePath(shaderDir+"fullscreen.vert"), resolvePath(shaderDir+"gravity_ripple.frag"));

    app.texStarGlow = loadTexture("resources/starGlow.png");
    app.texAtmosphere = loadTexture("resources/atmosphere.png");
    app.texStar = loadTexture("resources/star.png");
    app.texSurface = loadTexture("resources/surfacesHighRes.png");
    app.texSkydome = loadTexture("resources/skydomeFull.png");
    app.texLensFlare = loadTexture("resources/lensFlare.png");
    app.texStarCore = loadTexture("resources/starCore.png");
    app.texEclipseGlow = loadTexture("resources/eclipseGlow.png");
    app.texParticle = loadTexture("resources/particle.png");

    // Planet cloud textures (5 varieties for planet diversity)
    for (int i = 0; i < 5; i++) {
        app.texPlanetClouds[i] = loadTexture("resources/planetClouds" + std::to_string(i+1) + ".png");
    }

    app.bgStars.create(8000);
    app.billboard.create();
    app.sphereHi.create(48, 48);  // Higher quality spheres
    app.sphereMd.create(24, 24);
    app.sphereLo.create(12, 12);
    app.unitRing.create(1.0f, 128);
    app.ringDisc.create(0.5f, 1.0f, 64);  // Saturn ring annulus

    setupBloom(app);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#ifndef __ANDROID__
    glEnable(GL_PROGRAM_POINT_SIZE);  // ES 3.0 always enables this
    glEnable(GL_MULTISAMPLE);         // Not in ES 3.x
#endif
    return true;
}

// ============================================================
// BUILD SCENE
// ============================================================
void buildScene(App& app) {
    app.artistNodes.clear();
    int total = (int)app.library.artists.size();
    for (int i = 0; i < total; i++) {
        ArtistNode node;
        node.index = i;
        node.name = app.library.artists[i].name;
        node.totalTracks = app.library.artists[i].totalTracks;
        computeArtistColor(node);
        computeArtistPosition(node, total, app.library.artists[i].primaryGenre);
        computeAlbumOrbits(node, app.library.artists[i], i);
        node.glowRadius = node.radiusInit * (0.8f + std::min(node.totalTracks / 30.0f, 1.0f) * 1.2f);
        app.artistNodes.push_back(node);
    }
    float maxR = 0;
    for (auto& n : app.artistNodes) maxR = std::max(maxR, glm::length(n.pos));
    app.camera.targetOrbitDist = std::max(maxR * 1.5f, 50.0f);
    app.camera.orbitDist = app.camera.targetOrbitDist;
    app.statusMsg = std::to_string(total) + " artists, " +
                    std::to_string(app.library.totalAlbums) + " albums, " +
                    std::to_string(app.library.totalTracks) + " tracks";

    // Create GL textures for album art
    for (int ai = 0; ai < (int)app.library.artists.size(); ai++) {
        for (int bi = 0; bi < (int)app.library.artists[ai].albums.size(); bi++) {
            auto& album = app.library.artists[ai].albums[bi];
            if (!album.coverArtData.empty()) {
                int w, h, ch;
                unsigned char* img = stbi_load_from_memory(
                    album.coverArtData.data(), (int)album.coverArtData.size(), &w, &h, &ch, 4);
                if (img) {
                    GLuint tex;
                    glGenTextures(1, &tex);
                    glBindTexture(GL_TEXTURE_2D, tex);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
                    glGenerateMipmap(GL_TEXTURE_2D);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    stbi_image_free(img);

                    std::string key = std::to_string(ai) + "_" + std::to_string(bi);
                    app.albumArtTextures[key] = tex;
                }
                // Free the raw data since we have the GL texture now
                album.coverArtData.clear();
                album.coverArtData.shrink_to_fit();
            }
        }
    }
    std::cout << "[Planetary] Loaded " << app.albumArtTextures.size() << " album art textures" << std::endl;
}

// Forward declarations
void recenterToNowPlaying(App& app);

// ============================================================
// RENDERING
// ============================================================
// Project 3D position to screen coords for text labels
// ALL CAPS conversion
std::string toUpper(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::toupper);
    return r;
}

glm::vec2 worldToScreen(const glm::mat4& vp, glm::vec3 pos, int w, int h) {
    glm::vec4 clip = vp * glm::vec4(pos, 1.0f);
    if (clip.w <= 0.01f) return glm::vec2(-1000, -1000);
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * w, (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
}

// Helper: compute moon position with tilted orbit
glm::vec3 getMoonPos(glm::vec3 center, float radius, float angle, float tiltX, float tiltZ) {
    float x = cosf(angle) * radius;
    float z = sinf(angle) * radius;
    float y = x * sinf(tiltX) + z * sinf(tiltZ); // tilt the orbit plane
    x *= cosf(tiltX);
    z *= cosf(tiltZ);
    return center + glm::vec3(x, y, z);
}

void renderScene(App& app) {
    glm::mat4 view = app.camera.viewMatrix();
    glm::mat4 proj = app.camera.projMatrix();

    bool isZoomedToStar = (app.selectedArtist >= 0);

    // --- Background: pure black clear + dim point stars only ---
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);

    // Only show skydome at galaxy level (it washes out when zoomed in)
    if (!isZoomedToStar) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        app.planetShader.use();
        app.planetShader.setMat4("uView", glm::value_ptr(view));
        app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
        glBindTexture(GL_TEXTURE_2D, app.texSkydome);
        glm::mat4 skyM = glm::translate(glm::mat4(1.0f), app.camera.position);
        skyM = glm::scale(skyM, glm::vec3(900.0f));
        app.planetShader.setMat4("uModel", glm::value_ptr(skyM));
        app.planetShader.setVec3("uColor", 0.02f, 0.03f, 0.05f);
        app.planetShader.setVec3("uEmissive", 0.005f, 0.008f, 0.015f);
        app.planetShader.setFloat("uEmissiveStrength", 1.0f);
        app.planetShader.setVec3("uLightPos", 0, 0, 0);
        glCullFace(GL_FRONT); glEnable(GL_CULL_FACE);
        app.sphereLo.draw();
        glDisable(GL_CULL_FACE);
    }

    // Background point stars
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    app.starPointShader.use();
    app.starPointShader.setMat4("uView", glm::value_ptr(view));
    app.starPointShader.setMat4("uProjection", glm::value_ptr(proj));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app.texStar);
    app.starPointShader.setInt("uTexture", 0);
    app.bgStars.draw();

    // --- NEBULA CLOUDS --- rich Hubble-like gas clouds filling the galaxy
    // 3-layer system: large diffuse background, medium visible clouds, bright cores
    app.billboardShader.use();
    app.billboardShader.setMat4("uView", glm::value_ptr(view));
    app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
    {
        std::mt19937 nebRng(12345); // deterministic
        std::uniform_real_distribution<float> uni01(0.0f, 1.0f);
        std::uniform_real_distribution<float> uniPM(-1.0f, 1.0f);

        // Extended color palette matching Hubble imagery
        auto nebulaColor = [](float h) -> glm::vec3 {
            if (h < 0.12f) return glm::vec3(0.85f, 0.2f, 0.1f);       // Deep red
            if (h < 0.22f) return glm::vec3(0.9f, 0.35f, 0.15f);      // Salmon/orange
            if (h < 0.32f) return glm::vec3(0.7f, 0.25f, 0.12f);      // Dark red-brown
            if (h < 0.42f) return glm::vec3(0.55f, 0.15f, 0.55f);     // Purple/magenta
            if (h < 0.50f) return glm::vec3(0.3f, 0.15f, 0.6f);       // Deep violet
            if (h < 0.58f) return glm::vec3(0.15f, 0.3f, 0.75f);      // Deep blue
            if (h < 0.66f) return glm::vec3(0.1f, 0.5f, 0.8f);        // Bright cyan-blue
            if (h < 0.74f) return glm::vec3(0.1f, 0.55f, 0.5f);       // Teal
            if (h < 0.82f) return glm::vec3(0.8f, 0.5f, 0.15f);       // Warm orange
            if (h < 0.90f) return glm::vec3(0.9f, 0.7f, 0.4f);        // Gold
            return glm::vec3(0.6f, 0.15f, 0.4f);                       // Rose/pink
        };

        float audioGlow = app.audio.playing ? app.audioWave * 0.006f : 0;

        // === LAYER 1: Giant diffuse background nebulae ===
        // Very large, very subtle - creates overall color atmosphere
        glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
        for (int ci = 0; ci < 25; ci++) {
            float angle = uni01(nebRng) * 6.283f;
            float dist = 50.0f + uni01(nebRng) * 350.0f;
            float y = uniPM(nebRng) * 120.0f;
            glm::vec3 cpos(cosf(angle) * dist, y, sinf(angle) * dist);
            float csize = 150.0f + uni01(nebRng) * 350.0f;
            glm::vec3 col = nebulaColor(uni01(nebRng));
            float alpha = 0.006f + uni01(nebRng) * 0.008f;
            alpha += sinf(app.elapsedTime * 0.04f + ci * 0.8f) * 0.002f;
            alpha += audioGlow;
            app.billboard.draw(cpos, glm::vec4(col, alpha), csize);
        }

        // === LAYER 2: Nebula regions - clustered gas structures ===
        // 8 distinct nebula regions, each with a dominant color and 15-20 clouds
        glBindTexture(GL_TEXTURE_2D, app.texParticle);
        struct NebRegion { glm::vec3 center; float radius; float hueBase; };
        NebRegion regions[10]; // 10 regions: 4 red/warm, 3 blue/cyan, 3 mixed
        for (int r = 0; r < 10; r++) {
            float a = uni01(nebRng) * 6.283f;
            float d = 80.0f + uni01(nebRng) * 250.0f;
            regions[r].center = glm::vec3(cosf(a)*d, uniPM(nebRng)*60.0f, sinf(a)*d);
            regions[r].radius = 60.0f + uni01(nebRng) * 100.0f;
            regions[r].hueBase = uni01(nebRng);
        }
        // Force 3 regions to be distinctly blue/cyan (0.50-0.74 in palette)
        regions[1].hueBase = 0.54f; // Deep blue
        regions[4].hueBase = 0.62f; // Bright cyan-blue
        regions[7].hueBase = 0.70f; // Teal

        for (int r = 0; r < 10; r++) {
            int cloudsInRegion = 15 + (int)(uni01(nebRng) * 10.0f);
            for (int ci = 0; ci < cloudsInRegion; ci++) {
                // Scatter within region
                glm::vec3 offset(
                    uniPM(nebRng) * regions[r].radius,
                    uniPM(nebRng) * regions[r].radius * 0.4f,
                    uniPM(nebRng) * regions[r].radius
                );
                glm::vec3 cpos = regions[r].center + offset;
                float csize = 25.0f + uni01(nebRng) * 80.0f;

                // Color: mostly the region's dominant hue with variation
                float hue = fmodf(regions[r].hueBase + uniPM(nebRng) * 0.15f + 1.0f, 1.0f);
                glm::vec3 col = nebulaColor(hue);

                // Visible alpha - these are the main visible gas clouds
                float alpha = 0.012f + uni01(nebRng) * 0.025f;
                alpha += sinf(app.elapsedTime * 0.08f + (float)(r * 20 + ci) * 0.3f) * 0.004f;
                alpha += audioGlow;

                app.billboard.draw(cpos, glm::vec4(col, alpha), csize);
            }
        }

        // === LAYER 3: Bright cores and filament highlights ===
        // Smaller, brighter spots within nebula regions
        glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
        for (int r = 0; r < 10; r++) {
            int brightSpots = 4 + (int)(uni01(nebRng) * 6.0f);
            for (int ci = 0; ci < brightSpots; ci++) {
                glm::vec3 offset(
                    uniPM(nebRng) * regions[r].radius * 0.6f,
                    uniPM(nebRng) * regions[r].radius * 0.25f,
                    uniPM(nebRng) * regions[r].radius * 0.6f
                );
                glm::vec3 cpos = regions[r].center + offset;
                float csize = 15.0f + uni01(nebRng) * 40.0f;

                // Brighter, warmer colors for emission regions
                float hue = fmodf(regions[r].hueBase + uniPM(nebRng) * 0.1f + 1.0f, 1.0f);
                glm::vec3 col = nebulaColor(hue);
                col = glm::mix(col, glm::vec3(1.0f, 0.9f, 0.8f), 0.2f); // push toward white

                float alpha = 0.02f + uni01(nebRng) * 0.03f;
                alpha += sinf(app.elapsedTime * 0.15f + (float)(r * 10 + ci) * 0.7f) * 0.008f;
                alpha += audioGlow * 1.5f;

                app.billboard.draw(cpos, glm::vec4(col, alpha), csize);
            }
        }

        // === Interstellar gas wisps - elongated tendrils between regions ===
        glBindTexture(GL_TEXTURE_2D, app.texParticle);
        for (int wi = 0; wi < 30; wi++) {
            // Connect random pairs of regions with gas wisps
            int r1 = (int)(uni01(nebRng) * 9.99f);
            int r2 = (r1 + 1 + (int)(uni01(nebRng) * 8.99f)) % 10;
            float t = uni01(nebRng);
            glm::vec3 wpos = glm::mix(regions[r1].center, regions[r2].center, t);
            wpos += glm::vec3(uniPM(nebRng), uniPM(nebRng), uniPM(nebRng)) * 40.0f;
            float wsize = 20.0f + uni01(nebRng) * 50.0f;
            float hue = fmodf((regions[r1].hueBase + regions[r2].hueBase) * 0.5f +
                uniPM(nebRng) * 0.1f + 1.0f, 1.0f);
            glm::vec3 col = nebulaColor(hue);
            float alpha = 0.008f + uni01(nebRng) * 0.012f + audioGlow;
            app.billboard.draw(wpos, glm::vec4(col, alpha), wsize);
        }

        // === DARK MATTER WISPS - mysterious dim particles drifting through space ===
        glBindTexture(GL_TEXTURE_2D, app.texParticle);
        for (int di = 0; di < 150; di++) {
            float seed = (float)di * 7.31f;
            float angle = seed * 2.39996f; // golden angle
            float dist = 30.0f + fmodf(seed * 13.7f, 300.0f);
            float y = sinf(seed * 0.618f) * 100.0f;

            // Very slow drift
            float driftT = app.elapsedTime * 0.01f + seed;
            float dx = sinf(driftT * 0.3f + seed) * 5.0f;
            float dy = cosf(driftT * 0.2f + seed * 1.5f) * 3.0f;
            float dz = sinf(driftT * 0.25f + seed * 0.7f) * 5.0f;

            glm::vec3 dpos(cosf(angle) * dist + dx, y + dy, sinf(angle) * dist + dz);
            float dsize = 1.0f + fmodf(seed * 3.1f, 4.0f);

            // Dim blue-purple color
            float hv = fmodf(seed * 0.1f, 1.0f);
            glm::vec3 dmColor;
            if (hv < 0.4f) dmColor = glm::vec3(0.15f, 0.12f, 0.3f);      // Dark purple
            else if (hv < 0.7f) dmColor = glm::vec3(0.1f, 0.15f, 0.25f);  // Dark blue
            else dmColor = glm::vec3(0.12f, 0.1f, 0.18f);                   // Deep indigo

            float dmAlpha = 0.04f + sinf(driftT * 0.5f) * 0.015f;
            dmAlpha += audioGlow * 0.5f;
            app.billboard.draw(dpos, glm::vec4(dmColor, dmAlpha), dsize);
        }
    }

    // --- Star rendering ---
    app.billboardShader.use();
    app.billboardShader.setMat4("uView", glm::value_ptr(view));
    app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
    glBindTexture(GL_TEXTURE_2D, app.texStarGlow);

    for (auto& n : app.artistNodes) {
        if (n.isSelected) {
            // Selected star rendered below with planets
            continue;
        }

        // When zoomed to a star, SKIP all other star glows
        // This is the key fix -- 1300 overlapping additive glows = whiteout
        if (isZoomedToStar) continue;

        // Galaxy view: render as tiny colored spheres (not billboard circles)
        float distToCam = glm::length(n.pos - app.camera.position);
        if (distToCam > 800.0f) continue;
    }

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // --- Star rendering (solid spheres, not billboards) ---
    app.planetShader.use();
    app.planetShader.setMat4("uView", glm::value_ptr(view));
    app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
    app.planetShader.setVec3("uLightPos", 0, 50, 0);
    glBindTexture(GL_TEXTURE_2D, app.texStarCore);

    for (auto& n : app.artistNodes) {
        if (n.isSelected) {
            // SELECTED STAR: bright colored sphere + massive glow corona
            float starSize = n.radius * 0.35f;
            float pulse = 1.0f + app.audioWave * 0.1f;
            float coreSize = starSize * 0.8f * pulse;

            // Artist color is DOMINANT -- each star has its unique vibrant hue
            glm::vec3 starColor = n.color;
            glm::vec3 brightColor = glm::mix(starColor, glm::vec3(1.0f), 0.3f);

            // Bright colored sphere using planet shader (guaranteed to work on all GPUs)
            glBindTexture(GL_TEXTURE_2D, app.texStarCore);
            glm::mat4 m = glm::translate(glm::mat4(1.0f), n.pos);
            m = glm::rotate(m, app.elapsedTime * 0.15f, glm::vec3(0.05f, 1, 0));
            m = glm::scale(m, glm::vec3(coreSize));
            app.planetShader.setMat4("uModel", glm::value_ptr(m));
            // Color the sphere with the artist's color, bright
            app.planetShader.setVec3("uColor", brightColor.r, brightColor.g, brightColor.b);
            // High emissive = self-luminous, no dark side
            app.planetShader.setVec3("uEmissive", brightColor.r, brightColor.g, brightColor.b);
            app.planetShader.setFloat("uEmissiveStrength", 0.85f + app.audioWave * 0.15f);
            app.planetShader.setVec3("uLightPos", n.pos.x, n.pos.y, n.pos.z);
            app.sphereHi.draw();

            // === MASSIVE GLOW CORONA ===
            // THIS is what creates the "flame" look -- multiple layered billboard
            // glows around the sphere, using the starGlow texture's soft falloff
            glDepthMask(GL_FALSE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            app.billboardShader.use();
            app.billboardShader.setMat4("uView", glm::value_ptr(view));
            app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
            glBindTexture(GL_TEXTURE_2D, app.texStarGlow);

            float aPulse = 1.0f + app.audioWave * 0.2f;

            // Layer 1: Inner bright glow - white-hot, tight around sphere
            app.billboard.draw(n.pos,
                glm::vec4(brightColor * 0.9f + glm::vec3(0.1f), 0.45f * aPulse),
                coreSize * 3.5f);

            // Layer 2: Mid corona - artist-colored, the main visible flame halo
            app.billboard.draw(n.pos,
                glm::vec4(brightColor, 0.3f * aPulse),
                coreSize * 6.0f);

            // Layer 3: Outer corona - rich artist color, wide
            app.billboard.draw(n.pos,
                glm::vec4(starColor * 0.8f, 0.15f * aPulse),
                coreSize * 10.0f);

            // Layer 4: Outermost atmosphere - faint, very wide
            app.billboard.draw(n.pos,
                glm::vec4(starColor * 0.5f, 0.06f * aPulse),
                coreSize * 15.0f);

            // Offset glows for irregular flame edges (breaks the perfect circle)
            for (int gi = 0; gi < 8; gi++) {
                float seed = (float)gi * 137.508f + n.hue * 50.0f;
                float gAngle = app.elapsedTime * 0.06f + seed;
                float gOffset = coreSize * (0.5f + sinf(gAngle * 0.7f) * 0.3f);
                glm::vec3 gPos = n.pos + glm::vec3(
                    cosf(seed * 2.4f) * gOffset,
                    sinf(seed * 1.6f) * gOffset * 0.5f,
                    sinf(seed * 2.4f) * gOffset
                );
                float gSize = coreSize * (3.0f + sinf(gAngle) * 1.0f) * aPulse;
                float gAlpha = 0.12f + sinf(gAngle * 1.5f) * 0.04f;
                glm::vec3 gCol = glm::mix(brightColor, starColor, 0.5f);
                app.billboard.draw(gPos, glm::vec4(gCol, gAlpha), gSize);
            }

            // === MASSIVE SOLAR FLARES - shoot outward on the beat ===
            if (app.audio.playing && app.playingArtist == app.selectedArtist) {

                // Giant coronal mass ejections -- long streaming flares
                glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
                int numFlares = 8;
                for (int fi = 0; fi < numFlares; fi++) {
                    float seed = (float)fi * 137.508f + n.hue * 50.0f;

                    // Each flare has its own beat phase -- they fire at different times
                    float beatPhase = app.elapsedTime * (0.8f + (fi % 3) * 0.3f) + seed;
                    float lifecycle = fmodf(beatPhase * 0.2f, 1.0f);

                    // Sharp eruption driven by audio
                    float eruptPower = powf(sinf(lifecycle * (float)M_PI), 2.0f);
                    eruptPower *= app.audioBass; // Driven by BASS

                    if (eruptPower < 0.03f) continue;

                    // Direction from star surface (unique per flare)
                    float theta = seed * 2.39996f;
                    float phi = sinf(seed * 1.618f) * (float)M_PI;
                    glm::vec3 dir(
                        sinf(phi) * cosf(theta),
                        sinf(phi) * sinf(theta) * 0.7f,
                        cosf(phi)
                    );

                    // Stream of particles along the flare direction
                    int streamLen = 8 + (int)(eruptPower * 12.0f);
                    for (int si = 0; si < streamLen; si++) {
                        float t = (float)si / (float)streamLen; // 0=base, 1=tip
                        float dist = coreSize * (1.2f + t * eruptPower * 12.0f);

                        // Slight curve (magnetic field lines)
                        float curve = sinf(t * (float)M_PI) * coreSize * 0.5f;
                        glm::vec3 perp(dir.z * 0.3f, fabsf(dir.x) * 0.2f, -dir.x * 0.3f);
                        glm::vec3 flarePos = n.pos + dir * dist + perp * curve;

                        // Size: thick at base, thin at tip
                        float flareSize = coreSize * (0.5f + (1.0f - t) * 0.8f) * eruptPower;

                        // Color: white-hot at base -> artist color -> darker at tip
                        glm::vec3 flareCol;
                        if (t < 0.3f)
                            flareCol = glm::mix(glm::vec3(1.0f), brightColor, t / 0.3f);
                        else
                            flareCol = glm::mix(brightColor, starColor * 0.4f, (t - 0.3f) / 0.7f);

                        float flareAlpha = (1.0f - t * 0.7f) * eruptPower * 0.2f;
                        app.billboard.draw(flarePos, glm::vec4(flareCol, flareAlpha), flareSize);
                    }
                }

                // Smaller rapid-fire prominence sparks
                glBindTexture(GL_TEXTURE_2D, app.texParticle);
                for (int si = 0; si < 25; si++) {
                    float seed = (float)si * 73.13f + n.hue * 30.0f;
                    float sparkPhase = app.elapsedTime * (1.5f + (si % 5) * 0.5f) + seed;
                    float sparkLife = fmodf(sparkPhase * 0.4f, 1.0f);
                    float sparkPow = powf(sinf(sparkLife * (float)M_PI), 3.0f);
                    sparkPow *= (0.2f + app.audioWave * 0.8f);

                    if (sparkPow < 0.05f) continue;

                    float theta = seed * 2.39996f + app.elapsedTime * 0.1f;
                    float phi = sinf(seed * 1.618f) * (float)M_PI;
                    glm::vec3 dir(sinf(phi)*cosf(theta), sinf(phi)*sinf(theta), cosf(phi));

                    float dist = coreSize * (1.0f + sparkPow * 4.0f);
                    glm::vec3 sparkPos = n.pos + dir * dist;
                    float sparkSize = coreSize * (0.08f + sparkPow * 0.15f);

                    glm::vec3 sparkCol = glm::mix(
                        glm::vec3(1.0f, 0.95f, 0.8f), brightColor, sparkPow);
                    float sparkAlpha = sparkPow * 0.2f;

                    app.billboard.draw(sparkPos, glm::vec4(sparkCol, sparkAlpha), sparkSize);
                }

                // === ORBITAL PARTICLES - atoms/protons orbiting the star ===
                // Like electrons around a nucleus, driven by music
                glBindTexture(GL_TEXTURE_2D, app.texParticle);
                int numOrbitalParticles = 60;
                for (int pi = 0; pi < numOrbitalParticles; pi++) {
                    float seed = (float)pi * 137.508f + n.hue * 100.0f;

                    // Multiple orbital shells at different radii
                    int shell = pi % 5;
                    float orbitR = coreSize * (2.5f + (float)shell * 1.8f);

                    // Each particle has unique orbital plane and speed
                    float orbitSpeed = (0.3f + (float)(pi % 7) * 0.1f);
                    orbitSpeed *= (1.0f + app.audioWave * 0.3f); // Music speeds them up
                    float orbitAngle = app.elapsedTime * orbitSpeed + seed;

                    // Tilted orbital plane per particle
                    float tiltA = sinf(seed * 0.618f) * 1.2f;
                    float tiltB = cosf(seed * 0.314f) * 0.8f;

                    float px = cosf(orbitAngle) * orbitR;
                    float pz = sinf(orbitAngle) * orbitR;
                    float py = px * sinf(tiltA) * 0.4f + pz * sinf(tiltB) * 0.3f;
                    px *= cosf(tiltA * 0.3f);
                    pz *= cosf(tiltB * 0.3f);

                    glm::vec3 ppos = n.pos + glm::vec3(px, py, pz);
                    float psize = coreSize * (0.03f + 0.02f * sinf(seed * 2.0f));

                    // Bright cyan/white particles that pulse
                    float pBright = 0.5f + 0.5f * sinf(app.elapsedTime * 2.0f + seed);
                    pBright *= (0.5f + app.audioWave * 0.5f);
                    glm::vec3 pcolor = glm::mix(
                        glm::vec3(0.3f, 0.7f, 1.0f),  // Cyan
                        glm::vec3(1.0f, 0.9f, 0.7f),   // Warm white
                        sinf(seed) * 0.5f + 0.5f
                    );

                    app.billboard.draw(ppos, glm::vec4(pcolor, pBright * 0.15f), psize);
                }

                // === DARK MATTER RING - subtle ring of particles around the star system ===
                glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
                for (int dmi = 0; dmi < 30; dmi++) {
                    float seed = (float)dmi * 11.7f + n.hue * 30.0f;
                    float dmAngle = app.elapsedTime * 0.015f + seed * 0.5f;
                    float dmR = coreSize * (8.0f + sinf(seed * 2.0f) * 3.0f);
                    float dmY = sinf(dmAngle * 0.3f + seed) * coreSize * 1.5f;

                    glm::vec3 dmPos = n.pos + glm::vec3(
                        cosf(dmAngle) * dmR, dmY, sinf(dmAngle) * dmR);
                    float dmSize = coreSize * (0.15f + sinf(seed * 3.0f) * 0.08f);

                    glm::vec3 dmCol(0.12f, 0.1f, 0.22f); // Deep indigo
                    float dmA = 0.025f + sinf(app.elapsedTime * 0.3f + seed) * 0.01f;
                    dmA += app.audioWave * 0.01f;
                    app.billboard.draw(dmPos, glm::vec4(dmCol, dmA), dmSize);
                }
            }
            glDepthMask(GL_TRUE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            app.planetShader.use();
            app.planetShader.setMat4("uView", glm::value_ptr(view));
            app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
            app.planetShader.use();
            app.planetShader.setMat4("uView", glm::value_ptr(view));
            app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
        } else {
            // Non-selected: small colored sphere
            float cs = n.radius * 0.16f;
            glBindTexture(GL_TEXTURE_2D, app.texStarCore);
            glm::mat4 m = glm::translate(glm::mat4(1.0f), n.pos);
            m = glm::rotate(m, app.elapsedTime * 0.5f, glm::vec3(0,1,0));
            m = glm::scale(m, glm::vec3(cs));
            app.planetShader.setMat4("uModel", glm::value_ptr(m));
            glm::vec3 coreColor = glm::mix(n.color, glm::vec3(1.0f), 0.4f);
            app.planetShader.setVec3("uColor", coreColor.r, coreColor.g, coreColor.b);
            app.planetShader.setVec3("uEmissive", n.color.r, n.color.g, n.color.b);
            app.planetShader.setFloat("uEmissiveStrength", 0.5f);
            app.sphereLo.draw();
        }
    }

    // --- Selected artist: orbit rings + album planets ---
    if (app.selectedArtist >= 0 && app.selectedArtist < (int)app.artistNodes.size()) {
        auto& star = app.artistNodes[app.selectedArtist];

        // Orbit rings -- only show for selected album (cleaner look)
        if (app.selectedAlbum >= 0) {
            app.ringShader.use();
            app.ringShader.setMat4("uView", glm::value_ptr(view));
            app.ringShader.setMat4("uProjection", glm::value_ptr(proj));
            // Show the selected album's orbit ring
            auto& selO = star.albumOrbits[app.selectedAlbum];
            glm::mat4 rm = glm::translate(glm::mat4(1.0f), star.pos);
            rm = glm::scale(rm, glm::vec3(selO.radius));
            app.ringShader.setMat4("uModel", glm::value_ptr(rm));
            app.ringShader.setVec4("uColor", BRIGHT_BLUE.r, BRIGHT_BLUE.g, BRIGHT_BLUE.b, 0.08f);
            app.unitRing.draw();
        }

        // Album planets
        app.planetShader.use();
        app.planetShader.setMat4("uView", glm::value_ptr(view));
        app.planetShader.setMat4("uProjection", glm::value_ptr(proj));

        for (int ai = 0; ai < (int)star.albumOrbits.size(); ai++) {
            auto& o = star.albumOrbits[ai];
            float angle = o.angle + app.elapsedTime * o.speed;
            glm::vec3 apos = star.pos + glm::vec3(cosf(angle)*o.radius, 0, sinf(angle)*o.radius);

            // Planet color from hash
            std::hash<std::string> hasher;
            size_t albumHash = hasher(o.name);
            float planetHue = (float)(albumHash % 1000) / 1000.0f;
            float planetSat = 0.3f + (float)((albumHash >> 10) % 100) / 200.0f;
            float ph = planetHue * 6.0f;
            int phi = (int)ph % 6;
            float pf = ph - (int)ph;
            float pp = 1.0f - planetSat, pq = 1.0f - planetSat * pf, pt = 1.0f - planetSat * (1.0f - pf);
            glm::vec3 planetColor;
            switch (phi) {
                case 0: planetColor = {1, pt, pp}; break;
                case 1: planetColor = {pq, 1, pp}; break;
                case 2: planetColor = {pp, 1, pt}; break;
                case 3: planetColor = {pp, pq, 1}; break;
                case 4: planetColor = {pt, pp, 1}; break;
                default: planetColor = {1, pp, pq}; break;
            }

            // Unique tilt per planet
            float tiltX = sinf((float)albumHash * 0.1f) * 0.3f;
            float tiltZ = cosf((float)albumHash * 0.2f) * 0.25f;

            // === USE ALBUM ART AS PLANET TEXTURE if available ===
            std::string artKey = std::to_string(app.selectedArtist) + "_" + std::to_string(ai);
            auto artIt = app.albumArtTextures.find(artKey);
            bool hasArt = (artIt != app.albumArtTextures.end());

            if (hasArt) {
                // Album art texture -- use white color so art shows through
                glBindTexture(GL_TEXTURE_2D, artIt->second);
                app.planetShader.setVec3("uColor", 0.85f, 0.85f, 0.85f);
            } else {
                // Fallback: colored generic surface
                glBindTexture(GL_TEXTURE_2D, app.texSurface);
                app.planetShader.setVec3("uColor", planetColor.r * 0.7f, planetColor.g * 0.7f, planetColor.b * 0.7f);
            }

            glm::mat4 pm = glm::translate(glm::mat4(1.0f), apos);
            pm = glm::rotate(pm, app.elapsedTime * 0.12f + (float)ai * 1.5f,
                glm::vec3(tiltX, 1.0f, tiltZ));
            pm = glm::scale(pm, glm::vec3(o.planetSize));
            app.planetShader.setMat4("uModel", glm::value_ptr(pm));
            app.planetShader.setVec3("uLightPos", star.pos.x, star.pos.y, star.pos.z);
            app.planetShader.setVec3("uEmissive", 0.01f, 0.01f, 0.02f);
            app.planetShader.setFloat("uEmissiveStrength", ai == app.selectedAlbum ? 0.2f : 0.05f);
            app.sphereHi.draw();

            // Cloud layer (semi-transparent, slightly larger, slower rotation)
            if (o.numTracks > 3) {
                int cloudIdx = albumHash % 5;
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                // Use actual cloud textures with per-planet variety
                GLuint cloudTex = app.texPlanetClouds[cloudIdx];
                glBindTexture(GL_TEXTURE_2D, cloudTex ? cloudTex : app.texSurface);
                glm::mat4 cm = glm::translate(glm::mat4(1.0f), apos);
                cm = glm::rotate(cm, app.elapsedTime * 0.08f + (float)ai * 2.0f,
                    glm::vec3(tiltX * 0.5f, 1.0f, tiltZ * 0.7f));
                cm = glm::scale(cm, glm::vec3(o.planetSize * 1.02f));
                app.planetShader.setMat4("uModel", glm::value_ptr(cm));
                // Varied cloud tints per planet for diversity
                const glm::vec3 cloudTints[5] = {
                    {0.7f, 0.7f, 0.78f}, {0.78f, 0.72f, 0.65f},
                    {0.65f, 0.78f, 0.72f}, {0.72f, 0.65f, 0.78f},
                    {0.8f, 0.77f, 0.72f}
                };
                glm::vec3 cc = cloudTints[cloudIdx];
                app.planetShader.setVec3("uColor", cc.r, cc.g, cc.b);
                app.planetShader.setVec3("uEmissive", 0.0f, 0.0f, 0.0f);
                app.planetShader.setFloat("uEmissiveStrength", 0.0f);
                app.sphereHi.draw();
            }

            // Cyan-blue atmosphere ring -- pulses with audio
            glDepthMask(GL_FALSE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            app.billboardShader.use();
            app.billboardShader.setMat4("uView", glm::value_ptr(view));
            app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
            glBindTexture(GL_TEXTURE_2D, app.texAtmosphere);
            float audioPulse = app.audio.playing ? app.audioWave * 0.05f : 0;
            float atmoAlpha = ((ai == app.selectedAlbum) ? 0.2f : 0.1f) + audioPulse;
            app.billboard.draw(apos,
                glm::vec4(0.3f, 0.7f, 1.0f, atmoAlpha),
                o.planetSize * 2.5f);
            glDepthMask(GL_TRUE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Saturn-like rings for large albums (10+ tracks)
            if (o.numTracks >= 10 && app.saturnRingShader.id) {
                glDepthMask(GL_FALSE);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

                app.saturnRingShader.use();
                app.saturnRingShader.setMat4("uView", glm::value_ptr(view));
                app.saturnRingShader.setMat4("uProjection", glm::value_ptr(proj));

                float ringScale = o.planetSize * 2.5f;
                glm::mat4 rm = glm::translate(glm::mat4(1.0f), apos);
                rm = glm::rotate(rm, tiltX * 0.7f, glm::vec3(1, 0, 0));
                rm = glm::rotate(rm, tiltZ * 0.5f + 0.3f, glm::vec3(0, 0, 1));
                rm = glm::scale(rm, glm::vec3(ringScale));

                app.saturnRingShader.setMat4("uModel", glm::value_ptr(rm));
                app.saturnRingShader.setVec3("uColor", planetColor.r * 0.9f, planetColor.g * 0.85f, planetColor.b * 0.8f);
                app.saturnRingShader.setVec3("uLightPos", star.pos.x, star.pos.y, star.pos.z);
                app.saturnRingShader.setFloat("uAlpha", 0.65f);
                app.saturnRingShader.setFloat("uTime", app.elapsedTime);

                app.ringDisc.draw();

                glDepthMask(GL_TRUE);
                // Restore planet shader
                app.planetShader.use();
                app.planetShader.setMat4("uView", glm::value_ptr(view));
                app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
            }

            // Track moons -- only show when this album is selected
            if (ai == app.selectedAlbum) {
                // Draw tilted orbit rings for each moon
                app.ringShader.use();
                app.ringShader.setMat4("uView", glm::value_ptr(view));
                app.ringShader.setMat4("uProjection", glm::value_ptr(proj));
                for (auto& t : o.tracks) {
                    glm::mat4 trm = glm::translate(glm::mat4(1.0f), apos);
                    // Apply the same tilt as the moon's orbit
                    trm = glm::rotate(trm, t.tiltX, glm::vec3(1, 0, 0));
                    trm = glm::rotate(trm, t.tiltZ, glm::vec3(0, 0, 1));
                    trm = glm::scale(trm, glm::vec3(t.radius));
                    app.ringShader.setMat4("uModel", glm::value_ptr(trm));
                    app.ringShader.setVec4("uColor", BRIGHT_BLUE.r * 0.5f, BRIGHT_BLUE.g * 0.5f, BRIGHT_BLUE.b * 0.5f, 0.04f);
                    app.unitRing.draw();
                }
                app.planetShader.use();
                app.planetShader.setMat4("uView", glm::value_ptr(view));
                app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
                glBindTexture(GL_TEXTURE_2D, app.texSurface);
                for (int ti = 0; ti < (int)o.tracks.size(); ti++) {
                    auto& t = o.tracks[ti];
                    float ta = t.angle + app.elapsedTime * t.speed;
                    glm::vec3 mp = getMoonPos(apos, t.radius, ta, t.tiltX, t.tiltZ);
                    glm::mat4 mm = glm::translate(glm::mat4(1.0f), mp);
                    mm = glm::scale(mm, glm::vec3(t.size));
                    app.planetShader.setMat4("uModel", glm::value_ptr(mm));
                    app.planetShader.setVec3("uLightPos", star.pos.x, star.pos.y, star.pos.z);

                    bool isPlayingTrack = (app.playingArtist == app.selectedArtist &&
                        app.playingAlbum == ai && app.playingTrack == ti && app.audio.playing);
                    if (isPlayingTrack) {
                        // Playing track moon glows brighter
                        app.planetShader.setVec3("uColor", 0.8f, 0.9f, 1.0f);
                        app.planetShader.setVec3("uEmissive", BRIGHT_BLUE.r, BRIGHT_BLUE.g, BRIGHT_BLUE.b);
                        app.planetShader.setFloat("uEmissiveStrength", 0.4f);
                    } else {
                        app.planetShader.setVec3("uColor", 0.6f, 0.6f, 0.65f);
                        app.planetShader.setVec3("uEmissive", star.color.r * 0.1f, star.color.g * 0.1f, star.color.b * 0.1f);
                        app.planetShader.setFloat("uEmissiveStrength", 0.1f);
                    }
                    app.sphereMd.draw();

                    // Playback trail -- cyan arc showing track progress
                    if (isPlayingTrack) {
                        float progress = app.audio.progress();
                        int segments = std::max(4, (int)(progress * 80));
                        std::vector<float> trailVerts;
                        for (int s = 0; s <= segments; s++) {
                            float frac = (float)s / 80.0f;
                            float a = frac * 2.0f * (float)M_PI;
                            glm::vec3 tp = getMoonPos(apos, t.radius, a, t.tiltX, t.tiltZ);
                            trailVerts.push_back(tp.x);
                            trailVerts.push_back(tp.y + 0.01f);
                            trailVerts.push_back(tp.z);
                        }
                        GLuint trailVAO, trailVBO;
                        glGenVertexArrays(1, &trailVAO);
                        glGenBuffers(1, &trailVBO);
                        glBindVertexArray(trailVAO);
                        glBindBuffer(GL_ARRAY_BUFFER, trailVBO);
                        glBufferData(GL_ARRAY_BUFFER, trailVerts.size()*sizeof(float), trailVerts.data(), GL_STREAM_DRAW);
                        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), 0);
                        glEnableVertexAttribArray(0);

                        app.ringShader.use();
                        app.ringShader.setMat4("uView", glm::value_ptr(view));
                        app.ringShader.setMat4("uProjection", glm::value_ptr(proj));
                        glm::mat4 identity(1.0f);
                        app.ringShader.setMat4("uModel", glm::value_ptr(identity));
                        app.ringShader.setVec4("uColor", BRIGHT_BLUE.r, BRIGHT_BLUE.g, BRIGHT_BLUE.b, 0.8f);
                        glLineWidth(2.0f);
                        glDrawArrays(GL_LINE_STRIP, 0, segments + 1);
                        glLineWidth(1.0f);

                        glDeleteBuffers(1, &trailVBO);
                        glDeleteVertexArrays(1, &trailVAO);
                        glBindVertexArray(0);

                        // Restore planet shader
                        app.planetShader.use();
                        app.planetShader.setMat4("uView", glm::value_ptr(view));
                        app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
                        glBindTexture(GL_TEXTURE_2D, app.texSurface);
                    }
                }
            }
            app.planetShader.use();
            app.planetShader.setMat4("uView", glm::value_ptr(view));
            app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
            glBindTexture(GL_TEXTURE_2D, app.texSurface);
        }
    }
}

// ============================================================
// METEORS - random track shooting stars
// ============================================================
void updateMeteors(App& app, float dt) {
    if (app.artistNodes.empty()) return;

    // Spawn new meteors
    app.nextMeteorTime -= dt;
    if (app.nextMeteorTime <= 0) {
        app.nextMeteorTime = 2.0f + (float)(rand() % 60) / 10.0f; // 2-8 seconds

        // Pick a random track from the library
        if (!app.library.artists.empty()) {
            int ai = rand() % app.library.artists.size();
            auto& artist = app.library.artists[ai];
            if (!artist.albums.empty()) {
                int bi = rand() % artist.albums.size();
                auto& album = artist.albums[bi];
                if (!album.tracks.empty()) {
                    int ti = rand() % album.tracks.size();

                    App::Meteor m;
                    // Start from random edge of the galaxy
                    float angle = (float)(rand() % 1000) / 1000.0f * 2.0f * (float)M_PI;
                    float dist = 100.0f + (float)(rand() % 200);
                    float y = ((float)(rand() % 100) / 100.0f - 0.5f) * 80.0f;
                    m.pos = glm::vec3(cosf(angle) * dist, y, sinf(angle) * dist);

                    // Fly toward center-ish with some randomness
                    glm::vec3 target = glm::vec3(
                        ((float)(rand() % 100) / 100.0f - 0.5f) * 40.0f,
                        ((float)(rand() % 100) / 100.0f - 0.5f) * 20.0f,
                        ((float)(rand() % 100) / 100.0f - 0.5f) * 40.0f
                    );
                    m.vel = glm::normalize(target - m.pos) * (15.0f + (float)(rand() % 20));

                    // Color based on artist
                    if (ai < (int)app.artistNodes.size()) {
                        m.color = app.artistNodes[ai].glowColor;
                    } else {
                        m.color = BRIGHT_BLUE;
                    }
                    m.size = 0.15f + (float)(rand() % 100) / 500.0f;
                    m.maxLife = 3.0f + (float)(rand() % 30) / 10.0f;
                    m.life = m.maxLife;
                    m.trackName = album.tracks[ti].title;
                    app.meteors.push_back(m);
                }
            }
        }
    }

    // Update existing meteors
    for (auto& m : app.meteors) {
        m.trail.push_back(m.pos);
        if (m.trail.size() > 20) m.trail.erase(m.trail.begin());
        m.pos += m.vel * dt;
        m.life -= dt;
    }

    // Remove dead meteors
    app.meteors.erase(
        std::remove_if(app.meteors.begin(), app.meteors.end(),
            [](const App::Meteor& m) { return m.life <= 0; }),
        app.meteors.end()
    );
}

void renderMeteors(App& app) {
    if (app.meteors.empty()) return;
    glm::mat4 view = app.camera.viewMatrix();
    glm::mat4 proj = app.camera.projMatrix();

    for (auto& m : app.meteors) {
        float alpha = std::min(m.life / m.maxLife, 1.0f) * std::min((m.maxLife - m.life) / 0.5f, 1.0f);

        // Meteor head (additive glow)
        glDepthMask(GL_FALSE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        app.billboardShader.use();
        app.billboardShader.setMat4("uView", glm::value_ptr(view));
        app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
        glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
        app.billboard.draw(m.pos, glm::vec4(m.color, alpha * 0.4f), m.size * 3.0f);
        app.billboard.draw(m.pos, glm::vec4(1.0f, 1.0f, 1.0f, alpha * 0.6f), m.size * 1.0f);

        // Trail
        if (m.trail.size() >= 2) {
            std::vector<float> tv;
            for (auto& p : m.trail) { tv.push_back(p.x); tv.push_back(p.y); tv.push_back(p.z); }
            GLuint tvao, tvbo;
            glGenVertexArrays(1, &tvao); glGenBuffers(1, &tvbo);
            glBindVertexArray(tvao); glBindBuffer(GL_ARRAY_BUFFER, tvbo);
            glBufferData(GL_ARRAY_BUFFER, tv.size()*sizeof(float), tv.data(), GL_STREAM_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), 0);
            glEnableVertexAttribArray(0);

            app.ringShader.use();
            app.ringShader.setMat4("uView", glm::value_ptr(view));
            app.ringShader.setMat4("uProjection", glm::value_ptr(proj));
            glm::mat4 id(1.0f);
            app.ringShader.setMat4("uModel", glm::value_ptr(id));
            app.ringShader.setVec4("uColor", m.color.r, m.color.g, m.color.b, alpha * 0.3f);
            glDrawArrays(GL_LINE_STRIP, 0, (int)m.trail.size());

            glDeleteBuffers(1, &tvbo); glDeleteVertexArrays(1, &tvao);
        }

        glDepthMask(GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
}

// ============================================================
// COMETS - long-tailed particles from galaxy edges
// ============================================================
void updateComets(App& app, float dt) {
    if (app.artistNodes.empty()) return;

    app.nextCometTime -= dt;
    if (app.nextCometTime <= 0) {
        app.nextCometTime = 12.0f + (float)(rand() % 100) / 10.0f; // 12-22 seconds

        App::Comet c;
        float angle = (float)(rand() % 1000) / 1000.0f * 2.0f * (float)M_PI;
        float dist = 200.0f + (float)(rand() % 150);
        float y = ((float)(rand() % 100) / 100.0f - 0.5f) * 100.0f;
        c.pos = glm::vec3(cosf(angle) * dist, y, sinf(angle) * dist);

        // Fly toward galaxy center with gentle curve
        glm::vec3 target(
            ((float)(rand() % 100) / 100.0f - 0.5f) * 60.0f,
            ((float)(rand() % 100) / 100.0f - 0.5f) * 30.0f,
            ((float)(rand() % 100) / 100.0f - 0.5f) * 60.0f
        );
        c.vel = glm::normalize(target - c.pos) * (5.0f + (float)(rand() % 60) / 10.0f);

        // Perpendicular acceleration for curved path
        glm::vec3 up(0, 1, 0);
        glm::vec3 side = glm::cross(glm::normalize(c.vel), up);
        if (glm::length(side) < 0.01f) side = glm::vec3(1, 0, 0);
        side = glm::normalize(side);
        c.accel = side * ((float)(rand() % 100) / 100.0f - 0.5f) * 0.4f;

        // Icy comet colors
        float hueVar = (float)(rand() % 100) / 100.0f;
        if (hueVar < 0.4f) c.color = glm::vec3(0.6f, 0.85f, 1.0f);       // Blue-white
        else if (hueVar < 0.7f) c.color = glm::vec3(0.4f, 0.9f, 0.85f);   // Cyan
        else c.color = glm::vec3(0.8f, 0.7f, 1.0f);                        // Lavender

        c.headSize = 0.3f + (float)(rand() % 100) / 200.0f;
        c.maxLife = 15.0f + (float)(rand() % 100) / 10.0f;
        c.life = c.maxLife;
        app.comets.push_back(c);
    }

    // Update existing comets
    for (auto& c : app.comets) {
        c.tail.push_back(c.pos);
        if (c.tail.size() > 80) c.tail.erase(c.tail.begin());
        c.vel += c.accel * dt; // Curved path
        c.pos += c.vel * dt;
        c.life -= dt;
    }

    // Remove dead comets
    app.comets.erase(
        std::remove_if(app.comets.begin(), app.comets.end(),
            [](const App::Comet& c) { return c.life <= 0; }),
        app.comets.end()
    );
}

void renderComets(App& app) {
    if (app.comets.empty()) return;
    glm::mat4 view = app.camera.viewMatrix();
    glm::mat4 proj = app.camera.projMatrix();

    glDepthMask(GL_FALSE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);

    app.billboardShader.use();
    app.billboardShader.setMat4("uView", glm::value_ptr(view));
    app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));

    for (auto& c : app.comets) {
        float lifeA = std::min(c.life / c.maxLife, 1.0f) * std::min((c.maxLife - c.life) / 1.0f, 1.0f);

        // Bright comet nucleus
        glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
        app.billboard.draw(c.pos, glm::vec4(1.0f, 1.0f, 1.0f, lifeA * 0.7f), c.headSize * 2.0f);
        app.billboard.draw(c.pos, glm::vec4(c.color, lifeA * 0.4f), c.headSize * 5.0f);

        // Glowing tail particles
        glBindTexture(GL_TEXTURE_2D, app.texParticle);
        int tailLen = (int)c.tail.size();
        for (int i = 0; i < tailLen; i++) {
            float t = (float)i / (float)std::max(tailLen - 1, 1); // 0=oldest, 1=newest
            float tailAlpha = t * lifeA * 0.25f;
            float tailSize = c.headSize * (0.15f + t * 0.85f);
            // Color shifts from warm at tail end to cool at head
            glm::vec3 tailColor = glm::mix(glm::vec3(0.8f, 0.4f, 0.2f), c.color, t);
            // Render every other point for performance, but always render near head
            if (i % 2 == 0 || i > tailLen - 10)
                app.billboard.draw(c.tail[i], glm::vec4(tailColor, tailAlpha), tailSize);
        }
    }

    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void render(App& app) {
    // Direct to screen -- no FBO, no post-process (keeps GL state clean for ImGui/search)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, app.screenW, app.screenH);
    glClearColor(0.0f, 0.0f, 0.005f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    renderScene(app);
    renderMeteors(app);
    renderComets(app);

    // Clean GL state for ImGui
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(0);
}

// ============================================================
// TEXT LABELS - Project 3D positions to screen, draw with ImGui
// ============================================================
void renderLabels(App& app) {
    glm::mat4 vp = app.camera.projMatrix() * app.camera.viewMatrix();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    // Artist name labels
    for (auto& n : app.artistNodes) {
        float distToCam = glm::length(n.pos - app.camera.position);
        // Only show labels for nearby stars or selected star
        float labelDist = n.isSelected ? 9999.0f : (app.currentLevel == G_ALPHA_LEVEL ? 80.0f : 30.0f);
        if (distToCam > labelDist) continue;

        glm::vec2 sp = worldToScreen(vp, n.pos + glm::vec3(0, n.radius * 0.3f, 0), app.screenW, app.screenH);
        if (sp.x < -100 || sp.x > app.screenW + 100 || sp.y < -100 || sp.y > app.screenH + 100) continue;

        float alpha = std::clamp(1.0f - (distToCam / labelDist), 0.1f, 1.0f);
        // WHITE text like the original Planetary
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, alpha * 0.9f));
        ImU32 shadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, alpha * 0.7f));

        std::string upperName = toUpper(n.name);
        const char* name = upperName.c_str();
        // Large font for artist names
        ImFont* nf = app.fontLarge ? app.fontLarge : ImGui::GetFont();
        float nfs = app.fontLarge ? 28.0f : ImGui::GetFontSize();
        ImVec2 textSize = nf->CalcTextSizeA(nfs, FLT_MAX, 0.0f, name);
        ImVec2 pos(sp.x - textSize.x * 0.5f, sp.y - textSize.y - 4);

        // Shadow for readability
        dl->AddText(nf, nfs, ImVec2(pos.x + 1, pos.y + 1), shadow, name);
        dl->AddText(nf, nfs, pos, col, name);
    }

    // Album/track labels when zoomed in
    if (app.selectedArtist >= 0 && app.selectedArtist < (int)app.artistNodes.size()) {
        auto& star = app.artistNodes[app.selectedArtist];

        for (int ai = 0; ai < (int)star.albumOrbits.size(); ai++) {
            auto& o = star.albumOrbits[ai];
            float angle = o.angle + app.elapsedTime * o.speed;
            glm::vec3 apos = star.pos + glm::vec3(cosf(angle)*o.radius, 0, sinf(angle)*o.radius);

            glm::vec2 sp = worldToScreen(vp, apos + glm::vec3(0, o.planetSize * 1.5f, 0), app.screenW, app.screenH);
            if (sp.x < -100 || sp.x > app.screenW + 100) continue;

            float alpha = (ai == app.selectedAlbum) ? 1.0f : 0.7f;
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 1.0f, 1.0f, alpha));
            ImU32 shadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, alpha * 0.6f));

            std::string albumUpper = toUpper(o.name);
            // Medium font for album names
            ImFont* af = app.fontMedium ? app.fontMedium : ImGui::GetFont();
            float afs = app.fontMedium ? 20.0f : ImGui::GetFontSize();
            ImVec2 ts = af->CalcTextSizeA(afs, FLT_MAX, 0.0f, albumUpper.c_str());
            ImVec2 pos(sp.x - ts.x * 0.5f, sp.y - ts.y);
            dl->AddText(af, afs, ImVec2(pos.x + 1, pos.y + 1), shadow, albumUpper.c_str());
            dl->AddText(af, afs, pos, col, albumUpper.c_str());

            // Track moon labels for selected album
            if (ai == app.selectedAlbum) {
                for (auto& t : o.tracks) {
                    float ta = t.angle + app.elapsedTime * t.speed;
                    glm::vec3 mp = getMoonPos(apos, t.radius, ta, t.tiltX, t.tiltZ);
                    glm::vec2 msp = worldToScreen(vp, mp + glm::vec3(0, t.size * 2.0f, 0), app.screenW, app.screenH);
                    if (msp.x < 0 || msp.x > app.screenW) continue;

                    ImU32 tc = ImGui::ColorConvertFloat4ToU32(ImVec4(0.9f, 0.9f, 0.95f, 0.7f));
                    ImU32 ts2 = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.5f));
                    std::string trackUpper = toUpper(t.name);
                    // Small font for track names
                    ImFont* tf = app.fontSmall ? app.fontSmall : ImGui::GetFont();
                    float tfs = app.fontSmall ? 13.0f : ImGui::GetFontSize();
                    ImVec2 tts = tf->CalcTextSizeA(tfs, FLT_MAX, 0.0f, trackUpper.c_str());
                    ImVec2 tp(msp.x - tts.x * 0.5f, msp.y - tts.y);
                    dl->AddText(tf, tfs, ImVec2(tp.x + 1, tp.y + 1), ts2, trackUpper.c_str());
                    dl->AddText(tf, tfs, tp, tc, trackUpper.c_str());
                }
            }
        }
    }
}

// ============================================================
// UI OVERLAY (Dear ImGui)
// ============================================================
void renderUI(App& app) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Use the larger bold font
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[1]);

    // Left sidebar - title, search, artist/album/track browser
    float sidebarW = 320;
    float sidebarH = std::min((float)app.screenH - 80, 700.0f);
    ImGui::SetNextWindowPos(ImVec2(10, 10));
    ImGui::SetNextWindowSize(ImVec2(sidebarW, sidebarH));
    ImGui::Begin("##topbar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove);

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "PLANETARY");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 1.0f), "Native");

    if (!app.statusMsg.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.6f, 0.7f, 0.8f), "%s", app.statusMsg.c_str());
    }

    if (app.scanning) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Scanning... %d / %d",
            app.scanProgress.load(), app.scanTotal.load());
    }

    // Search with clickable results list -- ALWAYS works, even when zoomed into a star
    if (app.artistNodes.size() > 0) {
        ImGui::SetNextItemWidth(290);
        ImGui::InputTextWithHint("##search", "Search artists...", app.searchBuf, sizeof(app.searchBuf));

        // Show results whenever there's text (no matter what level you're at)
        if (strlen(app.searchBuf) > 1) {
            std::string q = app.searchBuf;
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            int shown = 0;
            for (int i = 0; i < (int)app.artistNodes.size() && shown < 15; i++) {
                std::string lower = app.artistNodes[i].name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.find(q) != std::string::npos) {
                    if (ImGui::Selectable(app.artistNodes[i].name.c_str(), false, 0, ImVec2(0, 20))) {
                        // Deselect current star, navigate to the searched one
                        if (app.selectedArtist >= 0) app.artistNodes[app.selectedArtist].isSelected = false;
                        app.selectedArtist = i;
                        app.selectedAlbum = -1;
                        app.artistNodes[i].isSelected = true;
                        app.currentLevel = G_ARTIST_LEVEL;
                        app.camera.autoRotate = false;
                        app.camera.flyTo(app.artistNodes[i].pos, app.artistNodes[i].idealCameraDist);
                        app.searchBuf[0] = '\0'; // Clear search after selection
                    }
                    shown++;
                }
            }
            if (shown == 0) {
                ImGui::TextColored(ImVec4(0.5f, 0.4f, 0.4f, 0.7f), "No matches");
            }
        }
    }

    // Selected artist info
    if (app.selectedArtist >= 0 && app.selectedArtist < (int)app.artistNodes.size()) {
        auto& star = app.artistNodes[app.selectedArtist];
        ImGui::Separator();
        ImGui::TextColored(ImVec4(star.color.r, star.color.g, star.color.b, 1.0f),
            "%s", star.name.c_str());
        ImGui::TextColored(ImVec4(0.5f, 0.6f, 0.7f, 0.8f),
            "%d albums, %d tracks", (int)star.albumOrbits.size(), star.totalTracks);

        // Album list with cover art
        for (int i = 0; i < (int)star.albumOrbits.size(); i++) {
            auto& album = star.albumOrbits[i];
            bool selected = (i == app.selectedAlbum);

            // Album art thumbnail
            std::string artKey = std::to_string(app.selectedArtist) + "_" + std::to_string(i);
            auto artIt = app.albumArtTextures.find(artKey);
            if (artIt != app.albumArtTextures.end()) {
                ImGui::Image((ImTextureID)(intptr_t)artIt->second, ImVec2(32, 32));
                ImGui::SameLine();
            }

            if (ImGui::Selectable(album.name.c_str(), selected, 0, ImVec2(0, 32))) {
                app.selectedAlbum = (app.selectedAlbum == i) ? -1 : i;
                app.currentLevel = (app.selectedAlbum >= 0) ? G_ALBUM_LEVEL : G_ARTIST_LEVEL;
            }

            // Track list for selected album
            if (i == app.selectedAlbum) {
                ImGui::Indent(12);
                for (int t = 0; t < (int)album.tracks.size(); t++) {
                    auto& track = album.tracks[t];
                    int mins = (int)track.duration / 60;
                    int secs = (int)track.duration % 60;

                    bool isPlaying = (app.audio.currentTrack == track.filePath && app.audio.playing);

                    // Highlight playing track, make all tracks clearly clickable
                    if (isPlaying) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 1.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.1f, 0.3f, 0.5f, 0.6f));
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.8f, 0.85f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.1f, 0.15f, 0.2f, 0.4f));
                    }

                    char label[512];
                    snprintf(label, sizeof(label), " %2d  %s", t + 1, track.name.c_str());

                    if (ImGui::Selectable(label, isPlaying, 0, ImVec2(0, 22))) {
                        app.audio.play(track.filePath, track.name, star.name, album.name, track.duration);
                        app.playingArtist = app.selectedArtist;
                        app.playingAlbum = i;
                        app.playingTrack = t;
                        // Select this album and zoom camera to the moon
                        app.selectedAlbum = i;
                        app.currentLevel = G_TRACK_LEVEL;
                        // Compute moon position and fly there
                        float a = album.angle + app.elapsedTime * album.speed;
                        glm::vec3 apos = star.pos + glm::vec3(cosf(a)*album.radius, 0, sinf(a)*album.radius);
                        float ta = track.angle + app.elapsedTime * track.speed;
                        glm::vec3 mpos = getMoonPos(apos, track.radius, ta, track.tiltX, track.tiltZ);
                        app.camera.flyTo(mpos, track.radius * 4.0f + 0.5f);
                        app.camera.autoRotate = false;
                    }

                    // Duration on same line, right-aligned
                    ImGui::SameLine(sidebarW - 70);
                    ImGui::TextColored(ImVec4(0.4f, 0.5f, 0.6f, 0.7f), "%d:%02d", mins, secs);

                    ImGui::PopStyleColor(2);
                }
                ImGui::Unindent(12);
                ImGui::Spacing();
            }
        }
    }
    ImGui::End();

    // Bottom bar - Now Playing
    if (!app.audio.currentTrackName.empty()) {
        float barH = 60;
        ImGui::SetNextWindowPos(ImVec2(0, (float)app.screenH - barH));
        ImGui::SetNextWindowSize(ImVec2((float)app.screenW, barH));
        ImGui::Begin("##nowplaying", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        // Play/pause button
        if (ImGui::Button(app.audio.playing ? "||" : ">", ImVec2(30, 30))) {
            app.audio.togglePause();
        }
        ImGui::SameLine();

        // Recenter button -- fly to now playing
        if (ImGui::Button("@", ImVec2(30, 30))) {
            recenterToNowPlaying(app);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fly to now playing (L3)");
        ImGui::SameLine();

        // Shuffle button -- plays a random track and spawns a meteor
        if (ImGui::Button("~", ImVec2(30, 30))) {
            if (!app.library.artists.empty()) {
                int ai = rand() % app.library.artists.size();
                auto& artist = app.library.artists[ai];
                if (!artist.albums.empty()) {
                    int bi = rand() % artist.albums.size();
                    auto& album = artist.albums[bi];
                    if (!album.tracks.empty()) {
                        int ti = rand() % album.tracks.size();
                        auto& track = album.tracks[ti];
                        app.audio.play(track.filePath, track.title, artist.name, album.name, track.duration);
                        app.playingArtist = ai;
                        app.playingAlbum = bi;
                        app.playingTrack = ti;
                    }
                }
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Shuffle");
        ImGui::SameLine();

        // Track info
        ImGui::BeginGroup();
        ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", app.audio.currentTrackName.c_str());
        ImGui::TextColored(ImVec4(0.5f, 0.6f, 0.7f, 0.8f), "%s - %s",
            app.audio.currentArtist.c_str(), app.audio.currentAlbum.c_str());
        ImGui::EndGroup();

        ImGui::SameLine(280);

        // Time display
        float ct = app.audio.currentTime();
        int cm = (int)ct / 60, cs = (int)ct % 60;
        ImGui::TextColored(ImVec4(0.5f, 0.7f, 0.9f, 0.9f), "%d:%02d", cm, cs);
        ImGui::SameLine();

        // Seekable progress slider -- drag to change position
        float prog = app.audio.progress();
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.1f, 0.15f, 0.2f, 0.8f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.15f, 0.2f, 0.3f, 0.9f));
        ImGui::SetNextItemWidth((float)app.screenW - 520);
        if (ImGui::SliderFloat("##seek", &prog, 0.0f, 1.0f, "")) {
            // Seek to new position
            if (app.audio.soundInit && app.audio.duration > 0) {
                float seekTime = prog * app.audio.duration;
                ma_uint32 sampleRate = ma_engine_get_sample_rate(&app.audio.engine);
                ma_sound_seek_to_pcm_frame(&app.audio.sound,
                    (ma_uint64)(seekTime * sampleRate));
            }
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        int dm = (int)app.audio.duration / 60, ds = (int)app.audio.duration % 60;
        ImGui::TextColored(ImVec4(0.4f, 0.5f, 0.6f, 0.7f), "%d:%02d", dm, ds);

        // Volume
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::SliderFloat("##vol", &app.audio.volume, 0.0f, 1.0f, "")) {
            app.audio.setVolume(app.audio.volume);
        }

        ImGui::End();
    }

    // Welcome screen (no library loaded)
    if (app.artistNodes.empty() && !app.scanning) {
        ImGui::SetNextWindowPos(ImVec2(app.screenW * 0.5f, app.screenH * 0.5f), 0, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400, 200));
        ImGui::Begin("##welcome", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        ImGui::SetCursorPosX((400 - ImGui::CalcTextSize("PLANETARY").x * 2) / 2);
        ImGui::SetWindowFontScale(2.0f);
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "PLANETARY");
        ImGui::SetWindowFontScale(1.0f);

        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.6f, 0.8f), "Visualize your music as a universe");
        ImGui::Spacing(); ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.8f, 1.0f), "Drag a music folder onto this window");
        ImGui::TextColored(ImVec4(0.4f, 0.5f, 0.6f, 0.6f), "or launch with: planetary.exe <folder>");
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.3f, 0.4f, 0.5f, 0.5f), "Supports MP3, FLAC, M4A, AAC, OGG, WAV");
        ImGui::End();
    }

    // Virtual keyboard for controller search (R3 to toggle)
    if (app.showVirtualKB) {
        static const char* vkbRows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
        static const char* vkbSpecial[] = { "SPC", "DEL", "CLR", "GO" };
        int numLetterRows = 4;

        float kbW = 520, kbH = 300;
        ImGui::SetNextWindowPos(ImVec2(app.screenW * 0.5f, app.screenH * 0.5f), 0, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(kbW, kbH));
        ImGui::Begin("##vkb", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        // Search input display
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "SEARCH");
        ImGui::SameLine();
        std::string displayText = app.vkbInput + "_";
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", displayText.c_str());

        // Show matching results count
        if (!app.vkbInput.empty()) {
            std::string q = app.vkbInput;
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            int matches = 0;
            std::string firstMatch;
            for (auto& n : app.artistNodes) {
                std::string lower = n.name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.find(q) != std::string::npos) {
                    if (matches == 0) firstMatch = n.name;
                    matches++;
                }
            }
            if (matches > 0) {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 0.9f), "%d matches - %s%s",
                    matches, firstMatch.c_str(), matches > 1 ? " ..." : "");
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 0.8f), "No matches");
            }
        }
        ImGui::Spacing();

        // Keyboard grid
        float btnSize = 40.0f;
        float btnPad = 4.0f;
        for (int r = 0; r < numLetterRows; r++) {
            int len = (int)strlen(vkbRows[r]);
            // Center each row
            float rowW = len * (btnSize + btnPad) - btnPad;
            float indent = (kbW - 24 - rowW) * 0.5f;
            if (indent > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);

            for (int c = 0; c < len; c++) {
                bool selected = (app.vkbRow == r && app.vkbCol == c);
                char label[4] = { vkbRows[r][c], '\0' };

                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.9f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.15f, 0.2f, 0.9f));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.8f, 0.9f, 1));
                }

                if (ImGui::Button(label, ImVec2(btnSize, btnSize))) {
                    // Mouse click on key
                    app.vkbInput += vkbRows[r][c];
                }
                ImGui::PopStyleColor(2);

                if (c < len - 1) ImGui::SameLine(0, btnPad);
            }
        }

        // Special row
        {
            float specBtnW = 80.0f;
            float rowW = 4 * (specBtnW + btnPad) - btnPad;
            float indent = (kbW - 24 - rowW) * 0.5f;
            if (indent > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);

            ImVec4 specColors[4] = {
                {0.15f, 0.2f, 0.3f, 0.9f},  // SPC
                {0.3f, 0.15f, 0.15f, 0.9f},  // DEL
                {0.3f, 0.2f, 0.1f, 0.9f},    // CLR
                {0.1f, 0.35f, 0.2f, 0.9f}    // GO
            };
            for (int c = 0; c < 4; c++) {
                bool selected = (app.vkbRow == 4 && app.vkbCol == c);
                if (selected) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.9f, 1.0f));
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, specColors[c]);
                }

                if (ImGui::Button(vkbSpecial[c], ImVec2(specBtnW, btnSize))) {
                    // Mouse click on special key
                    if (c == 0) app.vkbInput += ' ';
                    else if (c == 1 && !app.vkbInput.empty()) app.vkbInput.pop_back();
                    else if (c == 2) app.vkbInput.clear();
                    else if (c == 3) {
                        // GO - search: copy vkb input into the search buffer
                        strncpy(app.searchBuf, app.vkbInput.c_str(), sizeof(app.searchBuf) - 1);
                        app.searchBuf[sizeof(app.searchBuf) - 1] = '\0';
                        if (!app.vkbInput.empty()) {
                            std::string q = app.vkbInput;
                            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
                            for (int i = 0; i < (int)app.artistNodes.size(); i++) {
                                std::string lower = app.artistNodes[i].name;
                                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                                if (lower.find(q) != std::string::npos) {
                                    if (app.selectedArtist >= 0)
                                        app.artistNodes[app.selectedArtist].isSelected = false;
                                    app.selectedArtist = i;
                                    app.selectedAlbum = -1;
                                    app.artistNodes[i].isSelected = true;
                                    app.currentLevel = G_ARTIST_LEVEL;
                                    app.camera.autoRotate = false;
                                    app.camera.flyTo(app.artistNodes[i].pos,
                                        app.artistNodes[i].idealCameraDist);
                                    break;
                                }
                            }
                        }
                        app.showVirtualKB = false;
                    }
                }
                ImGui::PopStyleColor(1);
                if (c < 3) ImGui::SameLine(0, btnPad);
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.5f, 0.6f, 0.6f), "D-pad:move  A:type  B:back  R3:close");
        ImGui::End();
    }

    // Controller hints (bottom-right, only when controller connected)
    if (app.controller && !app.showVirtualKB) {
        ImGui::SetNextWindowPos(ImVec2((float)app.screenW - 10, (float)app.screenH - 70), 0, ImVec2(1.0f, 1.0f));
        ImGui::Begin("##ctrlhints", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
        ImGui::TextColored(ImVec4(0.35f, 0.45f, 0.55f, 0.5f), "L3:Now Playing  R3:Search  LB/RB:Tracks");
        ImGui::End();
    }

    // GPU info (bottom right)
    ImGui::SetNextWindowPos(ImVec2((float)app.screenW - 10, 10), 0, ImVec2(1.0f, 0.0f));
    ImGui::Begin("##gpu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs);
    ImGui::TextColored(ImVec4(0.3f, 0.4f, 0.5f, 0.5f), "%s", (const char*)glGetString(GL_RENDERER));
    ImGui::End();

    app.imguiWantsMouse = ImGui::GetIO().WantCaptureMouse;

    // Render 3D text labels via ImGui draw lists
    renderLabels(app);

    ImGui::PopFont();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// ============================================================
// HIT TEST
// ============================================================
int hitTestStar(App& app, int mx, int my) {
    glm::mat4 view = app.camera.viewMatrix();
    glm::mat4 proj = app.camera.projMatrix();
    float bestDist = 999999; int bestIdx = -1;
    for (int i = 0; i < (int)app.artistNodes.size(); i++) {
        auto& n = app.artistNodes[i];
        // No search filter here -- all stars are always clickable
        // Search only affects the dropdown results in the sidebar
        glm::vec4 clip = proj * view * glm::vec4(n.pos, 1);
        if (clip.w <= 0) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float sx = (ndc.x * 0.5f + 0.5f) * app.screenW;
        float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * app.screenH;
        float d = sqrtf((sx-mx)*(sx-mx) + (sy-my)*(sy-my));
        float hitR = std::max(25.0f, n.glowRadius * 5.0f / std::max(clip.w * 0.1f, 0.1f));
        if (d < hitR && d < bestDist) { bestDist = d; bestIdx = i; }
    }
    return bestIdx;
}

// Hit test planets and moons in the 3D view. Returns {albumIdx, trackIdx} or {-1,-1}
struct HitResult { int album = -1; int track = -1; };

HitResult hitTestPlanetMoon(App& app, int mx, int my) {
    if (app.selectedArtist < 0) return {};
    auto& star = app.artistNodes[app.selectedArtist];
    glm::mat4 vp = app.camera.projMatrix() * app.camera.viewMatrix();
    float bestDist = 999999;
    HitResult result;

    for (int ai = 0; ai < (int)star.albumOrbits.size(); ai++) {
        auto& o = star.albumOrbits[ai];
        float angle = o.angle + app.elapsedTime * o.speed;
        glm::vec3 apos = star.pos + glm::vec3(cosf(angle)*o.radius, 0, sinf(angle)*o.radius);

        // Test planet (generous hit area for easier clicking)
        glm::vec2 sp = worldToScreen(vp, apos, app.screenW, app.screenH);
        float d = sqrtf((sp.x-mx)*(sp.x-mx) + (sp.y-my)*(sp.y-my));
        float hitR = std::max(35.0f, o.planetSize * 100.0f);
        if (d < hitR && d < bestDist) { bestDist = d; result = {ai, -1}; }

        // Test track moons (only for selected album, larger hit area)
        if (ai == app.selectedAlbum) {
            for (int ti = 0; ti < (int)o.tracks.size(); ti++) {
                auto& t = o.tracks[ti];
                float ta = t.angle + app.elapsedTime * t.speed;
                glm::vec3 mp = getMoonPos(apos, t.radius, ta, t.tiltX, t.tiltZ);
                glm::vec2 msp = worldToScreen(vp, mp, app.screenW, app.screenH);
                float md = sqrtf((msp.x-mx)*(msp.x-mx) + (msp.y-my)*(msp.y-my));
                float mhitR = std::max(35.0f, t.size * 180.0f);
                if (md < mhitR && md < bestDist) { bestDist = md; result = {ai, ti}; }
            }
        }
    }
    return result;
}

// ============================================================
// RECENTER TO NOW PLAYING - fly camera to the currently playing track
// ============================================================
void recenterToNowPlaying(App& app) {
    if (app.playingArtist < 0 || app.playingArtist >= (int)app.artistNodes.size()) return;

    auto& star = app.artistNodes[app.playingArtist];

    // Deselect current star
    if (app.selectedArtist >= 0 && app.selectedArtist < (int)app.artistNodes.size())
        app.artistNodes[app.selectedArtist].isSelected = false;

    // Select the playing artist
    app.selectedArtist = app.playingArtist;
    star.isSelected = true;
    app.selectedAlbum = app.playingAlbum;
    app.currentLevel = G_TRACK_LEVEL;
    app.camera.autoRotate = false;
    app.searchBuf[0] = '\0';

    // Compute the playing moon's current position and fly there
    if (app.playingAlbum >= 0 && app.playingAlbum < (int)star.albumOrbits.size()) {
        auto& album = star.albumOrbits[app.playingAlbum];
        float a = album.angle + app.elapsedTime * album.speed;
        glm::vec3 apos = star.pos + glm::vec3(cosf(a)*album.radius, 0, sinf(a)*album.radius);

        if (app.playingTrack >= 0 && app.playingTrack < (int)album.tracks.size()) {
            auto& track = album.tracks[app.playingTrack];
            float ta = track.angle + app.elapsedTime * track.speed;
            glm::vec3 mpos = getMoonPos(apos, track.radius, ta, track.tiltX, track.tiltZ);
            app.camera.flyTo(mpos, track.radius * 4.0f + 0.5f);
        } else {
            float outerTrack = album.tracks.empty() ? album.planetSize * 5.0f :
                album.tracks.back().radius * 2.5f;
            app.camera.flyTo(apos, std::max(outerTrack, 2.0f));
        }
    } else {
        app.camera.flyTo(star.pos, star.idealCameraDist);
    }
}

// ============================================================
// EVENTS
// ============================================================
void handleEvents(App& app) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        switch (ev.type) {
        case SDL_QUIT: app.running = false; break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                app.screenW = ev.window.data1; app.screenH = ev.window.data2;
                app.camera.aspect = (float)app.screenW / (float)app.screenH;
                glViewport(0, 0, app.screenW, app.screenH);
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (!app.imguiWantsMouse) {
                app.mouseDown = true;
                app.mouseButton = ev.button.button;
                app.mouseDragDist = 0;
                app.mouseDownX = ev.button.x;
                app.mouseDownY = ev.button.y;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (!app.imguiWantsMouse && app.mouseDown) {
                // Only treat as a CLICK if drag distance is small
                if (app.mouseDragDist < 8 && app.mouseButton == SDL_BUTTON_LEFT) {
                    int mx = ev.button.x, my = ev.button.y;
                    // Try planets/moons first
                    HitResult pmHit = hitTestPlanetMoon(app, mx, my);
                    if (pmHit.track >= 0 && pmHit.album >= 0) {
                        // Clicked a track moon -- play it and zoom to it
                        auto& star = app.artistNodes[app.selectedArtist];
                        auto& album = star.albumOrbits[pmHit.album];
                        auto& track = album.tracks[pmHit.track];
                        app.audio.play(track.filePath, track.name, star.name, album.name, track.duration);
                        app.playingArtist = app.selectedArtist;
                        app.playingAlbum = pmHit.album;
                        app.playingTrack = pmHit.track;
                        app.currentLevel = G_TRACK_LEVEL;
                        // Zoom camera close to the moon
                        float a = album.angle + app.elapsedTime * album.speed;
                        glm::vec3 apos = star.pos + glm::vec3(cosf(a)*album.radius, 0, sinf(a)*album.radius);
                        float ta = track.angle + app.elapsedTime * track.speed;
                        glm::vec3 mpos = apos + glm::vec3(cosf(ta)*track.radius, 0, sinf(ta)*track.radius);
                        app.camera.flyTo(mpos, track.radius * 4.0f + 0.5f);
                    } else if (pmHit.album >= 0) {
                        // Clicked an album planet -- select and zoom to it
                        app.selectedAlbum = (app.selectedAlbum == pmHit.album) ? -1 : pmHit.album;
                        app.currentLevel = (app.selectedAlbum >= 0) ? G_ALBUM_LEVEL : G_ARTIST_LEVEL;
                        // Zoom camera to the planet
                        if (app.selectedAlbum >= 0) {
                            auto& star = app.artistNodes[app.selectedArtist];
                            auto& album = star.albumOrbits[app.selectedAlbum];
                            float a = album.angle + app.elapsedTime * album.speed;
                            glm::vec3 apos = star.pos + glm::vec3(cosf(a)*album.radius, 0, sinf(a)*album.radius);
                            // Zoom close enough to see track moons
                            float outerTrack = album.tracks.empty() ? album.planetSize * 5.0f :
                                album.tracks.back().radius * 2.5f;
                            app.camera.flyTo(apos, std::max(outerTrack, 2.0f));
                        } else {
                            // Deselected album, zoom back to star
                            auto& star = app.artistNodes[app.selectedArtist];
                            app.camera.flyTo(star.pos, star.idealCameraDist);
                        }
                    } else {
                        int hit = hitTestStar(app, mx, my);
                        if (hit >= 0) {
                            // Clear search when clicking a star directly
                            app.searchBuf[0] = '\0';
                            if (app.selectedArtist >= 0) app.artistNodes[app.selectedArtist].isSelected = false;
                            if (hit == app.selectedArtist) {
                                app.selectedArtist = -1; app.selectedAlbum = -1;
                                app.currentLevel = G_ALPHA_LEVEL; app.camera.autoRotate = true;
                                float maxR = 0;
                                for (auto& n : app.artistNodes) maxR = std::max(maxR, glm::length(n.pos));
                                app.camera.flyTo(glm::vec3(0), maxR * 1.5f);
                            } else {
                                app.selectedArtist = hit; app.selectedAlbum = -1;
                                app.artistNodes[hit].isSelected = true;
                                app.currentLevel = G_ARTIST_LEVEL; app.camera.autoRotate = false;
                                app.camera.flyTo(app.artistNodes[hit].pos, app.artistNodes[hit].idealCameraDist);
                            }
                        }
                    }
                }
            }
            app.mouseDown = false;
            break;
        case SDL_MOUSEMOTION:
            if (app.mouseDown && !app.imguiWantsMouse) {
                app.mouseDragDist += abs(ev.motion.xrel) + abs(ev.motion.yrel);
                // LEFT or RIGHT click drag = orbit camera
                if (app.mouseButton == SDL_BUTTON_LEFT || app.mouseButton == SDL_BUTTON_RIGHT) {
                    app.camera.onMouseDrag((float)ev.motion.xrel, (float)ev.motion.yrel);
                    app.camera.autoRotate = false;
                }
            }
            break;
        case SDL_MOUSEWHEEL:
            if (!app.imguiWantsMouse) app.camera.onMouseScroll((float)ev.wheel.y);
            break;
        case SDL_KEYDOWN:
            if (!ImGui::GetIO().WantCaptureKeyboard) {
                if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    if (app.selectedAlbum >= 0) { app.selectedAlbum = -1; app.currentLevel = G_ARTIST_LEVEL; }
                    else if (app.selectedArtist >= 0) {
                        app.artistNodes[app.selectedArtist].isSelected = false;
                        app.selectedArtist = -1; app.currentLevel = G_ALPHA_LEVEL;
                        app.camera.autoRotate = true;
                        float maxR = 0;
                        for (auto& n : app.artistNodes) maxR = std::max(maxR, glm::length(n.pos));
                        app.camera.flyTo(glm::vec3(0), maxR * 1.5f);
                    } else { app.running = false; }
                }
                if (ev.key.keysym.sym == SDLK_SPACE) app.audio.togglePause();
                if (ev.key.keysym.sym == SDLK_n) recenterToNowPlaying(app);
            }
            break;
        case SDL_DROPFILE: {
#ifndef __ANDROID__
            char* path = ev.drop.file;
            if (fs::is_directory(path)) {
                app.musicPath = path;
                app.scanning = true;
                std::thread([&app]() {
                    app.library = scanMusicLibrary(app.musicPath,
                        [&](int d, int t) { app.scanProgress = d; app.scanTotal = t; });
                    app.libraryLoaded = true; app.scanning = false;
                }).detach();
            }
            SDL_free(path);
#endif
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                // A button = select (like left click at screen center)
                int cx = app.screenW / 2, cy = app.screenH / 2;
                if (app.selectedArtist >= 0) {
                    // At star level: try to select a planet or moon
                    HitResult pmHit = hitTestPlanetMoon(app, cx, cy);
                    if (pmHit.track >= 0 && pmHit.album >= 0) {
                        auto& star = app.artistNodes[app.selectedArtist];
                        auto& album = star.albumOrbits[pmHit.album];
                        auto& track = album.tracks[pmHit.track];
                        app.audio.play(track.filePath, track.name, star.name, album.name, track.duration);
                        app.playingArtist = app.selectedArtist;
                        app.playingAlbum = pmHit.album;
                        app.playingTrack = pmHit.track;
                        app.currentLevel = G_TRACK_LEVEL;
                    } else if (pmHit.album >= 0) {
                        app.selectedAlbum = (app.selectedAlbum == pmHit.album) ? -1 : pmHit.album;
                        app.currentLevel = (app.selectedAlbum >= 0) ? G_ALBUM_LEVEL : G_ARTIST_LEVEL;
                        if (app.selectedAlbum >= 0) {
                            auto& star = app.artistNodes[app.selectedArtist];
                            auto& album = star.albumOrbits[app.selectedAlbum];
                            float a = album.angle + app.elapsedTime * album.speed;
                            glm::vec3 apos = star.pos + glm::vec3(cosf(a)*album.radius, 0, sinf(a)*album.radius);
                            float outerTrack = album.tracks.empty() ? album.planetSize * 5.0f :
                                album.tracks.back().radius * 2.5f;
                            app.camera.flyTo(apos, std::max(outerTrack, 2.0f));
                        }
                    }
                } else {
                    // At galaxy level: select nearest visible star
                    int hit = hitTestStar(app, cx, cy);
                    if (hit >= 0) {
                        app.selectedArtist = hit;
                        app.selectedAlbum = -1;
                        app.artistNodes[hit].isSelected = true;
                        app.currentLevel = G_ARTIST_LEVEL;
                        app.camera.autoRotate = false;
                        app.camera.flyTo(app.artistNodes[hit].pos, app.artistNodes[hit].idealCameraDist);
                    }
                }
            }
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
                // B button = back (like ESC)
                if (app.selectedAlbum >= 0) {
                    app.selectedAlbum = -1;
                    app.currentLevel = G_ARTIST_LEVEL;
                    auto& star = app.artistNodes[app.selectedArtist];
                    app.camera.flyTo(star.pos, star.idealCameraDist);
                } else if (app.selectedArtist >= 0) {
                    app.artistNodes[app.selectedArtist].isSelected = false;
                    app.selectedArtist = -1;
                    app.currentLevel = G_ALPHA_LEVEL;
                    app.camera.autoRotate = true;
                    float maxR = 0;
                    for (auto& n : app.artistNodes) maxR = std::max(maxR, glm::length(n.pos));
                    app.camera.flyTo(glm::vec3(0), maxR * 1.5f);
                }
            }
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_Y) {
                app.audio.togglePause(); // Y = play/pause
            }
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_X) {
                // X = shuffle
                if (!app.library.artists.empty()) {
                    int ai = rand() % app.library.artists.size();
                    auto& artist = app.library.artists[ai];
                    if (!artist.albums.empty()) {
                        int bi = rand() % artist.albums.size();
                        auto& album = artist.albums[bi];
                        if (!album.tracks.empty()) {
                            int ti = rand() % album.tracks.size();
                            app.audio.play(album.tracks[ti].filePath, album.tracks[ti].title, artist.name, album.name, album.tracks[ti].duration);
                        }
                    }
                }
            }
            // L3 (click left stick) = recenter to now playing
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK) {
                recenterToNowPlaying(app);
            }
            // R3 (click right stick) = toggle virtual keyboard for search
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
                app.showVirtualKB = !app.showVirtualKB;
                if (app.showVirtualKB) {
                    app.vkbRow = 1; app.vkbCol = 0;
                    app.vkbInput = "";
                }
            }
            // L/R bumpers = prev/next track
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER) {
                if (app.playingArtist >= 0 && app.playingAlbum >= 0 && app.playingTrack > 0) {
                    auto& star = app.artistNodes[app.playingArtist];
                    auto& album = star.albumOrbits[app.playingAlbum];
                    int prev = app.playingTrack - 1;
                    auto& track = album.tracks[prev];
                    app.audio.play(track.filePath, track.name, star.name, album.name, track.duration);
                    app.playingTrack = prev;
                }
            }
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
                if (app.playingArtist >= 0 && app.playingAlbum >= 0) {
                    auto& star = app.artistNodes[app.playingArtist];
                    auto& album = star.albumOrbits[app.playingAlbum];
                    int next = app.playingTrack + 1;
                    if (next < (int)album.tracks.size()) {
                        auto& track = album.tracks[next];
                        app.audio.play(track.filePath, track.name, star.name, album.name, track.duration);
                        app.playingTrack = next;
                    }
                }
            }
            // D-pad: virtual keyboard navigation OR album navigation
            if (app.showVirtualKB) {
                // Virtual keyboard layout rows
                static const char* vkbLetters[] = {
                    "1234567890",
                    "QWERTYUIOP",
                    "ASDFGHJKL",
                    "ZXCVBNM",
                    nullptr  // special row: SPC DEL CLR GO
                };
                int numRows = 5;
                auto rowLen = [&](int r) -> int {
                    if (r == 4) return 4; // special buttons
                    return (int)strlen(vkbLetters[r]);
                };

                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    app.vkbRow = std::max(0, app.vkbRow - 1);
                    app.vkbCol = std::min(app.vkbCol, rowLen(app.vkbRow) - 1);
                }
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    app.vkbRow = std::min(numRows - 1, app.vkbRow + 1);
                    app.vkbCol = std::min(app.vkbCol, rowLen(app.vkbRow) - 1);
                }
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT) {
                    app.vkbCol = std::max(0, app.vkbCol - 1);
                }
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
                    app.vkbCol = std::min(rowLen(app.vkbRow) - 1, app.vkbCol + 1);
                }
                // A = type selected key
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                    if (app.vkbRow < 4) {
                        // Letter/number key
                        char c = vkbLetters[app.vkbRow][app.vkbCol];
                        app.vkbInput += c;
                    } else {
                        // Special row
                        switch (app.vkbCol) {
                            case 0: app.vkbInput += ' '; break;  // SPACE
                            case 1: // DELETE
                                if (!app.vkbInput.empty()) app.vkbInput.pop_back();
                                break;
                            case 2: app.vkbInput.clear(); break;  // CLEAR
                            case 3: // GO (search)
                                strncpy(app.searchBuf, app.vkbInput.c_str(), sizeof(app.searchBuf) - 1);
                                app.searchBuf[sizeof(app.searchBuf) - 1] = '\0';
                                // Find first matching artist and navigate to it
                                if (!app.vkbInput.empty()) {
                                    std::string q = app.vkbInput;
                                    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
                                    for (int i = 0; i < (int)app.artistNodes.size(); i++) {
                                        std::string lower = app.artistNodes[i].name;
                                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                                        if (lower.find(q) != std::string::npos) {
                                            if (app.selectedArtist >= 0)
                                                app.artistNodes[app.selectedArtist].isSelected = false;
                                            app.selectedArtist = i;
                                            app.selectedAlbum = -1;
                                            app.artistNodes[i].isSelected = true;
                                            app.currentLevel = G_ARTIST_LEVEL;
                                            app.camera.autoRotate = false;
                                            app.camera.flyTo(app.artistNodes[i].pos,
                                                app.artistNodes[i].idealCameraDist);
                                            break;
                                        }
                                    }
                                }
                                app.showVirtualKB = false;
                                break;
                        }
                    }
                }
                // B = backspace when in keyboard
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_B) {
                    if (!app.vkbInput.empty())
                        app.vkbInput.pop_back();
                    else
                        app.showVirtualKB = false; // Close if empty
                }
            } else {
                // Normal D-pad: navigate albums/tracks
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_UP) {
                    if (app.selectedArtist >= 0 && app.selectedAlbum > 0) {
                        app.selectedAlbum--;
                        app.currentLevel = G_ALBUM_LEVEL;
                    }
                }
                if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
                    if (app.selectedArtist >= 0) {
                        auto& star = app.artistNodes[app.selectedArtist];
                        if (app.selectedAlbum < (int)star.albumOrbits.size() - 1) {
                            app.selectedAlbum++;
                            app.currentLevel = G_ALBUM_LEVEL;
                        }
                    }
                }
            }
            // Start = toggle auto-rotate
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                app.camera.autoRotate = !app.camera.autoRotate;
            }
            // Guide/Home = toggle auto-rotate (Steam button)
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE) {
                app.camera.autoRotate = !app.camera.autoRotate;
            }
            // Back/Select = return to galaxy
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                if (app.selectedArtist >= 0) app.artistNodes[app.selectedArtist].isSelected = false;
                app.selectedArtist = -1;
                app.selectedAlbum = -1;
                app.currentLevel = G_ALPHA_LEVEL;
                app.camera.autoRotate = true;
                float maxR = 0;
                for (auto& n : app.artistNodes) maxR = std::max(maxR, glm::length(n.pos));
                app.camera.flyTo(glm::vec3(0), maxR * 1.5f);
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (app.controller) {
                SDL_GameControllerClose(app.controller);
                app.controller = nullptr;
                std::cout << "[Controller] Disconnected" << std::endl;
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!app.controller && SDL_IsGameController(ev.cdevice.which)) {
                app.controller = SDL_GameControllerOpen(ev.cdevice.which);
                if (app.controller)
                    std::cout << "[Controller] Connected: " << SDL_GameControllerName(app.controller) << std::endl;
            }
            break;
        }
    }
}

// ============================================================
// MAIN
// ============================================================
#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    int argc = __argc;
    char** argv = __argv;
#else
int main(int argc, char* argv[]) {
#endif
    App app;
    if (!initSDL(app)) return 1;
    if (!initResources(app)) return 1;

    // Load saved config first (persistent library)
    std::string savedPath = loadConfig();

    if (argc > 1) {
        app.musicPath = argv[1];
    } else if (!savedPath.empty()) {
        app.musicPath = savedPath;
        std::cout << "[Planetary] Auto-loading saved library: " << savedPath << std::endl;
    }

#ifdef __ANDROID__
    // Android: Always load from Navidrome server (Mac Studio LAN IP)
    if (app.musicPath.empty()) {
        app.musicPath = "http://10.0.0.73:4533"; // Navidrome server
    }
    app.scanning = true;
    std::thread([&app]() {
        app.library = fetchMusicLibraryFromNavidrome(app.musicPath,
            [&](int d, int t) { app.scanProgress = d; app.scanTotal = t; });
        app.libraryLoaded = true; app.scanning = false;
    }).detach();
#else
    if (!app.musicPath.empty() && fs::is_directory(app.musicPath)) {
        app.scanning = true;
        std::thread([&app]() {
            app.library = scanMusicLibrary(app.musicPath,
                [&](int d, int t) { app.scanProgress = d; app.scanTotal = t; });
            app.libraryLoaded = true; app.scanning = false;
        }).detach();
    }
#endif

    auto prev = std::chrono::high_resolution_clock::now();
    while (app.running) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        app.elapsedTime += dt;

        handleEvents(app);
        if (app.libraryLoaded.exchange(false)) {
            buildScene(app);
            saveConfig(app); // Remember this library for next launch
        }

        // Rebuild bloom FBOs on resize
        if (app.screenW / 2 != app.bloomW || app.screenH / 2 != app.bloomH) {
            setupBloom(app);
        }

        // Audio analysis for reactive visuals
        updateAudioAnalysis(app, dt);

        // Gamepad input (PS5/Xbox/Steam controller via SDL2)
        // Hot-plug: reconnect if controller was disconnected
        if (!app.controller) {
            for (int i = 0; i < SDL_NumJoysticks(); i++) {
                if (SDL_IsGameController(i)) {
                    app.controller = SDL_GameControllerOpen(i);
                    if (app.controller) {
                        std::cout << "[Controller] Connected: " << SDL_GameControllerName(app.controller) << std::endl;
                        break;
                    }
                }
            }
        }

        if (app.controller) {
            float lx = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0f;
            float ly = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0f;
            float rx = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.0f;
            float ry = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.0f;
            float lt = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32768.0f;
            float rt = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32768.0f;

            // Dead zone
            if (fabsf(lx) < 0.15f) lx = 0;
            if (fabsf(ly) < 0.15f) ly = 0;
            if (fabsf(rx) < 0.12f) rx = 0;
            if (fabsf(ry) < 0.12f) ry = 0;

            // Left stick: pan camera target (navigate the galaxy)
            if (lx != 0 || ly != 0) {
                // Move camera target in the camera's local XZ plane
                glm::vec3 forward = glm::normalize(app.camera.target - app.camera.position);
                glm::vec3 right = glm::normalize(glm::cross(forward, app.camera.up));
                glm::vec3 camUp = glm::normalize(glm::cross(right, forward));
                float panSpeed = app.camera.orbitDist * 0.02f;
                app.camera.targetLookAt += right * lx * panSpeed + camUp * (-ly) * panSpeed;
                app.camera.autoRotate = false;
            }

            // Right stick: orbit camera
            if (rx != 0 || ry != 0) {
                app.camera.onMouseDrag(rx * 8.0f, ry * 8.0f);
                app.camera.autoRotate = false;
            }
            // Triggers: zoom in/out
            if (rt > 0.1f) app.camera.onMouseScroll(rt * 2.0f);
            if (lt > 0.1f) app.camera.onMouseScroll(-lt * 2.0f);
        }

        // Auto-play next track when current one ends
        if (app.audio.soundInit && app.audio.isAtEnd() && app.playingArtist >= 0) {
            auto& star = app.artistNodes[app.playingArtist];
            if (app.playingAlbum >= 0 && app.playingAlbum < (int)star.albumOrbits.size()) {
                auto& album = star.albumOrbits[app.playingAlbum];
                int nextTrack = app.playingTrack + 1;
                if (nextTrack < (int)album.tracks.size()) {
                    auto& track = album.tracks[nextTrack];
                    app.audio.play(track.filePath, track.name, star.name, album.name, track.duration);
                    app.playingTrack = nextTrack;
                }
            }
        }

        app.camera.update(dt);
        updateMeteors(app, dt);
        updateComets(app, dt);
        render(app);     // includes renderScene + renderMeteors + renderComets + gravity ripple
        renderUI(app);   // also calls renderLabels inside ImGui frame
        SDL_GL_SwapWindow(app.window);
    }

    app.audio.cleanup();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(app.glContext);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
