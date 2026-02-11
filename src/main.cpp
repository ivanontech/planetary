// ============================================================
// PLANETARY - Native C++ / OpenGL
// Ported from the original Bloom Studio Planetary (Cinder/iOS)
// Music scanning from the Electron version
// ============================================================

#include <GL/glew.h>
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

#include "stb_image.h"
#include "shader.h"
#include "camera.h"
#include "music_data.h"
#include "miniaudio.h"

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
        orbit.speed = 0.04f / sqrtf(std::max(orbit.radius, 0.5f)); // Slower, more majestic
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
            // Orbit speed: shorter tracks orbit FASTER (but all visibly move)
            // Scale so even a 5-min track completes an orbit in ~20 seconds
            to.speed = (2.0f * (float)M_PI) / (std::max(to.duration, 30.0f) * 0.5f);
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

    void init() {
        ma_engine_config config = ma_engine_config_init();
        if (ma_engine_init(&config, &engine) != MA_SUCCESS) {
            std::cerr << "[Audio] Failed to init miniaudio engine" << std::endl;
            return;
        }
        engineInit = true;
        ma_engine_set_volume(&engine, volume);
        std::cout << "[Audio] miniaudio engine ready (MP3/FLAC/WAV/OGG)" << std::endl;
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

        if (ma_sound_init_from_file(&engine, path.c_str(), 0, nullptr, nullptr, &sound) != MA_SUCCESS) {
            std::cerr << "[Audio] Failed to load: " << path << std::endl;
            return;
        }
        soundInit = true;
        ma_sound_start(&sound);
        playing = true;
        std::cout << "[Audio] Playing: " << name << " by " << artist << std::endl;
        // Decode for visual analysis (separate from playback)
        decodeForAnalysis(path);
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

    // Audio analysis buffer
    std::vector<float> analysisBuffer;
    ma_uint32 analysisSampleRate = 44100;
    ma_uint64 analysisTotalFrames = 0;

    void decodeForAnalysis(const std::string& path) {
        ma_decoder decoder;
        ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32, 1, 44100);
        if (ma_decoder_init_file(path.c_str(), &decoderConfig, &decoder) != MA_SUCCESS) {
            std::cout << "[Audio] Analysis decode failed: " << path << std::endl;
            return;
        }
        ma_uint64 totalFrames;
        ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
        analysisTotalFrames = totalFrames;
        analysisSampleRate = 44100;
        analysisBuffer.resize(totalFrames);
        ma_uint64 framesRead;
        ma_decoder_read_pcm_frames(&decoder, analysisBuffer.data(), totalFrames, &framesRead);
        ma_decoder_uninit(&decoder);
        std::cout << "[Audio] Analysis: " << framesRead << " frames decoded" << std::endl;
    }

    // Get audio samples at current position for analysis
    float getRMSAtCursor(int windowSize = 1024) {
        if (analysisBuffer.empty() || !soundInit) return 0;
        float cursor = 0;
        ma_sound_get_cursor_in_seconds(&sound, &cursor);
        ma_uint64 pos = (ma_uint64)(cursor * analysisSampleRate);
        if (pos + windowSize >= analysisTotalFrames) return 0;
        float sum = 0;
        for (int i = 0; i < windowSize; i++) {
            float s = analysisBuffer[pos + i];
            sum += s * s;
        }
        return sqrtf(sum / windowSize);
    }

    // Simple bass detection (average of low-frequency energy)
    float getBassAtCursor(int windowSize = 2048) {
        if (analysisBuffer.empty() || !soundInit) return 0;
        float cursor = 0;
        ma_sound_get_cursor_in_seconds(&sound, &cursor);
        ma_uint64 pos = (ma_uint64)(cursor * analysisSampleRate);
        if (pos + windowSize >= analysisTotalFrames) return 0;
        // Simple low-pass: average consecutive sample differences
        float sum = 0;
        float prev = analysisBuffer[pos];
        int count = 0;
        for (int i = 1; i < windowSize; i += 4) { // subsample for bass
            float s = analysisBuffer[pos + i];
            float avg = (prev + s) * 0.5f; // low-pass
            sum += avg * avg;
            prev = s;
            count++;
        }
        return sqrtf(sum / std::max(count, 1));
    }

    // Treble detection
    float getTrebleAtCursor(int windowSize = 512) {
        if (analysisBuffer.empty() || !soundInit) return 0;
        float cursor = 0;
        ma_sound_get_cursor_in_seconds(&sound, &cursor);
        ma_uint64 pos = (ma_uint64)(cursor * analysisSampleRate);
        if (pos + windowSize >= analysisTotalFrames) return 0;
        // High-pass: sum of sample-to-sample differences
        float sum = 0;
        for (int i = 1; i < windowSize; i++) {
            float diff = analysisBuffer[pos + i] - analysisBuffer[pos + i - 1];
            sum += diff * diff;
        }
        return sqrtf(sum / windowSize) * 4.0f; // amplify treble
    }

    void cleanup() {
        if (soundInit) ma_sound_uninit(&sound);
        if (engineInit) ma_engine_uninit(&engine);
        analysisBuffer.clear();
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
    std::string searchQuery;

    // Rendering
    Shader starPointShader, billboardShader, planetShader, ringShader, starShader;
    Shader bloomBrightShader, bloomBlurShader, bloomCompositeShader;
    GLuint texStarGlow=0, texAtmosphere=0, texStar=0, texSurface=0, texSkydome=0;
    GLuint texLensFlare=0, texStarCore=0, texEclipseGlow=0, texParticle=0;
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
        if (!musicPath.empty() && fs::is_directory(musicPath)) {
            std::cout << "[Config] Loaded: " << musicPath << std::endl;
            return musicPath;
        }
    }
    return "";
}

// ============================================================
// AUDIO ANALYSIS - extract volume/frequency for reactive visuals
// ============================================================
void updateAudioAnalysis(App& app, float dt) {
    if (!app.audio.soundInit || !app.audio.playing) {
        app.audioLevel *= 0.92f;
        app.audioPeak *= 0.95f;
        app.audioBass *= 0.92f;
        app.audioWave *= 0.94f;
        return;
    }

    // REAL audio analysis from decoded buffer
    float rms = app.audio.getRMSAtCursor(1024);
    float bass = app.audio.getBassAtCursor(2048);
    float treble = app.audio.getTrebleAtCursor(512);

    // Smooth the values for visual appeal
    float targetLevel = std::min(rms * 3.0f, 1.0f);
    float targetBass = std::min(bass * 4.0f, 1.0f);

    app.audioLevel += (targetLevel - app.audioLevel) * 8.0f * dt;
    app.audioPeak = std::max(app.audioPeak * 0.96f, app.audioLevel);
    app.audioBass += (targetBass - app.audioBass) * 6.0f * dt;
    app.audioWave += (app.audioLevel - app.audioWave) * 10.0f * dt;
}

// ============================================================
// INIT
// ============================================================
bool initSDL(App& app) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return false;
    }
    initBasePath();

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
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

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { std::cerr << "GLEW init failed" << std::endl; return false; }

    std::cout << "[Planetary] OpenGL " << glGetString(GL_VERSION) << std::endl;
    std::cout << "[Planetary] GPU: " << glGetString(GL_RENDERER) << std::endl;

    // Init Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Load Montserrat Bold -- clean, modern font similar to Proxima Nova
    std::string fontPath = resolvePath("resources/Montserrat-Bold.ttf");
    ImFont* defaultFont = io.Fonts->AddFontDefault();
    ImFont* boldFont = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f);
    if (!boldFont) {
        std::cerr << "[Planetary] Failed to load font, using default" << std::endl;
        ImFontConfig cfg; cfg.SizePixels = 16.0f;
        boldFont = io.Fonts->AddFontDefault(&cfg);
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
    ImGui_ImplOpenGL3_Init("#version 330");

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
    if (!app.starPointShader.load(resolvePath("shaders/star_points.vert"), resolvePath("shaders/star_points.frag"))) return false;
    if (!app.billboardShader.load(resolvePath("shaders/billboard.vert"), resolvePath("shaders/billboard.frag"))) return false;
    if (!app.planetShader.load(resolvePath("shaders/planet.vert"), resolvePath("shaders/planet.frag"))) return false;
    if (!app.ringShader.load(resolvePath("shaders/orbit_ring.vert"), resolvePath("shaders/orbit_ring.frag"))) return false;
    if (!app.starShader.load(resolvePath("shaders/star.vert"), resolvePath("shaders/star.frag"))) return false;
    if (!app.bloomBrightShader.load(resolvePath("shaders/fullscreen.vert"), resolvePath("shaders/bloom_bright.frag"))) return false;
    if (!app.bloomBlurShader.load(resolvePath("shaders/fullscreen.vert"), resolvePath("shaders/bloom_blur.frag"))) return false;
    if (!app.bloomCompositeShader.load(resolvePath("shaders/fullscreen.vert"), resolvePath("shaders/bloom_composite.frag"))) return false;

    app.texStarGlow = loadTexture("resources/starGlow.png");
    app.texAtmosphere = loadTexture("resources/atmosphere.png");
    app.texStar = loadTexture("resources/star.png");
    app.texSurface = loadTexture("resources/surfacesHighRes.png");
    app.texSkydome = loadTexture("resources/skydomeFull.png");
    app.texLensFlare = loadTexture("resources/lensFlare.png");
    app.texStarCore = loadTexture("resources/starCore.png");
    app.texEclipseGlow = loadTexture("resources/eclipseGlow.png");
    app.texParticle = loadTexture("resources/particle.png");

    app.bgStars.create(8000);
    app.billboard.create();
    app.sphereHi.create(48, 48);  // Higher quality spheres
    app.sphereMd.create(24, 24);
    app.sphereLo.create(12, 12);
    app.unitRing.create(1.0f, 128);

    setupBloom(app);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_MULTISAMPLE);
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

    // --- NEBULA CLOUDS --- colorful gas clouds scattered in deep space
    // Creates the Hubble-like nebula atmosphere
    app.billboardShader.use();
    app.billboardShader.setMat4("uView", glm::value_ptr(view));
    app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
    glBindTexture(GL_TEXTURE_2D, app.texParticle);
    {
        // Seed-based nebula clouds at fixed positions
        std::mt19937 nebRng(12345); // deterministic
        std::uniform_real_distribution<float> nebDist(-400.0f, 400.0f);
        std::uniform_real_distribution<float> nebSize(60.0f, 200.0f);
        std::uniform_real_distribution<float> nebHue(0.0f, 1.0f);

        int numClouds = 80;
        for (int ci = 0; ci < numClouds; ci++) {
            glm::vec3 cpos(nebDist(nebRng), nebDist(nebRng) * 0.4f, nebDist(nebRng));
            float csize = nebSize(nebRng);
            float ch = nebHue(nebRng);

            // Nebula colors: blues, purples, reds, oranges
            glm::vec3 nebColor;
            if (ch < 0.3f) nebColor = glm::vec3(0.2f, 0.3f, 0.8f);       // Blue
            else if (ch < 0.5f) nebColor = glm::vec3(0.5f, 0.2f, 0.7f);   // Purple
            else if (ch < 0.7f) nebColor = glm::vec3(0.8f, 0.2f, 0.15f);  // Red
            else if (ch < 0.85f) nebColor = glm::vec3(0.7f, 0.4f, 0.15f); // Orange
            else nebColor = glm::vec3(0.15f, 0.6f, 0.5f);                  // Teal

            // Very subtle -- just enough to tint the darkness
            float nebAlpha = 0.02f + sinf(app.elapsedTime * 0.1f + ci * 0.5f) * 0.002f;

            // Audio reactivity -- nebulae glow slightly with music
            if (app.audio.playing) {
                nebAlpha += app.audioWave * 0.01f;
            }

            app.billboard.draw(cpos, glm::vec4(nebColor, nebAlpha), csize);
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
            // SELECTED STAR: bright sphere with tight contained glow
            float starSize = n.radius * 0.35f;

            // Star sphere with starCore texture - the MAIN bright element
            // Star: PROCEDURAL BURNING PLASMA surface
            float pulse = 1.0f + app.audioWave * 0.1f;
            float coreSize = starSize * 0.8f * pulse;
            app.starShader.use();
            app.starShader.setMat4("uView", glm::value_ptr(view));
            app.starShader.setMat4("uProjection", glm::value_ptr(proj));
            app.starShader.setFloat("uTime", app.elapsedTime);
            glBindTexture(GL_TEXTURE_2D, app.texStarCore);
            glm::mat4 m = glm::translate(glm::mat4(1.0f), n.pos);
            m = glm::rotate(m, app.elapsedTime * 0.1f, glm::vec3(0.05f, 1, 0));
            m = glm::scale(m, glm::vec3(coreSize));
            app.starShader.setMat4("uModel", glm::value_ptr(m));
            // Each star gets a UNIQUE color mix based on artist
            glm::vec3 starColor = glm::mix(n.color, glm::vec3(1.0f, 0.95f, 0.85f), 0.35f);
            app.starShader.setVec3("uColor", starColor.r, starColor.g, starColor.b);
            // Pass artist hue to shader for unique plasma palette
            app.starShader.setVec3("uEmissive", n.color.r * 0.6f, n.color.g * 0.5f, n.color.b * 0.4f);
            app.starShader.setFloat("uEmissiveStrength", 0.6f + app.audioWave * 0.5f);
            app.sphereHi.draw();
            // Switch back to planet shader for rest of scene
            app.planetShader.use();
            app.planetShader.setMat4("uView", glm::value_ptr(view));
            app.planetShader.setMat4("uProjection", glm::value_ptr(proj));

            // Smooth glow halo -- pulses with audio
            glDepthMask(GL_FALSE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            app.billboardShader.use();
            app.billboardShader.setMat4("uView", glm::value_ptr(view));
            app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
            glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
            float glowPulse = 0.06f + app.audioWave * 0.04f;
            app.billboard.draw(n.pos, glm::vec4(starColor * 0.5f, glowPulse), coreSize * 2.5f);
            // Audio-reactive gravity waves -- concentric expanding rings
            if (app.audio.playing && app.playingArtist == app.selectedArtist) {
                glBindTexture(GL_TEXTURE_2D, app.texAtmosphere);
                for (int w = 0; w < 3; w++) {
                    float waveT = fmodf(app.elapsedTime * 0.5f + (float)w * 1.0f, 3.0f) / 3.0f;
                    float waveR = coreSize * (2.0f + waveT * 12.0f);
                    float waveA = (1.0f - waveT) * 0.03f * app.audioWave;
                    app.billboard.draw(n.pos,
                        glm::vec4(starColor * 0.3f, waveA), waveR);
                }

                // SOLAR FLARES -- organic flame tongues erupting with the beat
                glBindTexture(GL_TEXTURE_2D, app.texParticle);
                int numFlares = 35;
                for (int fi = 0; fi < numFlares; fi++) {
                    float seed = (float)fi * 137.508f + n.hue * 50.0f;
                    
                    // Each flare has its own beat-driven lifecycle
                    float beatSync = app.audioBass * 2.0f + app.audioWave * 1.5f;
                    float phase = app.elapsedTime * (0.5f + (fi % 7) * 0.2f) + seed;
                    float lifecycle = fmodf(phase * 0.25f, 1.0f);
                    float erupt = sinf(lifecycle * (float)M_PI);
                    erupt *= (0.2f + beatSync * 0.4f);
                    
                    if (erupt < 0.03f) continue;
                    
                    // Organic direction -- each flare follows a curved path
                    float theta = seed * 2.39996f + sinf(app.elapsedTime * 0.3f + fi) * 0.3f;
                    float phi = sinf(seed * 1.618f) * (float)M_PI + cosf(app.elapsedTime * 0.2f) * 0.2f;
                    glm::vec3 dir(sinf(phi)*cosf(theta), sinf(phi)*sinf(theta)*0.7f, cosf(phi));
                    
                    // Multiple particles per flare to create a flame tongue
                    for (int p = 0; p < 3; p++) {
                        float pDist = coreSize * (1.0f + erupt * (1.5f + p * 1.2f));
                        float pSize = coreSize * (0.12f - p * 0.03f) * (0.5f + erupt);
                        float pAlpha = (0.08f + erupt * 0.06f) * (1.0f - p * 0.25f);
                        
                        // Slight offset for organic feel
                        glm::vec3 offset(sinf(seed+p)*0.05f, cosf(seed*2+p)*0.05f, sinf(seed*3+p)*0.05f);
                        glm::vec3 fpos = n.pos + dir * pDist + offset * coreSize;
                        
                        // Color gradient: hot white -> star color -> deep orange/red
                        glm::vec3 col;
                        if (p == 0) col = glm::vec3(1.0f, 0.95f, 0.85f);
                        else if (p == 1) col = glm::mix(starColor, glm::vec3(1.0f, 0.6f, 0.2f), 0.5f);
                        else col = glm::vec3(0.9f, 0.3f, 0.08f);
                        
                        app.billboard.draw(fpos, glm::vec4(col, pAlpha), pSize);
                    }
                }
                
                // === NEBULA GAS + GRAVITY LOOPS + WAVELENGTHS ===
                
                // Nebula gas clouds -- 8 unique colors
                glm::vec3 nebColors[] = {
                    glm::mix(n.color, glm::vec3(0.2f, 0.4f, 1.0f), 0.7f),
                    glm::mix(n.color, glm::vec3(0.9f, 0.15f, 0.5f), 0.6f),
                    glm::mix(n.color, glm::vec3(1.0f, 0.5f, 0.05f), 0.5f),
                    glm::mix(n.color, glm::vec3(0.2f, 0.9f, 0.7f), 0.6f),
                    glm::mix(n.color, glm::vec3(0.7f, 0.15f, 0.9f), 0.6f),
                    glm::vec3(1.0f, 0.8f, 0.3f),
                    glm::vec3(0.3f, 0.7f, 1.0f),
                    glm::vec3(0.9f, 0.25f, 0.25f),
                };
                
                glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
                // Gas jets -- shoot outward with bass
                for (int gi = 0; gi < 16; gi++) {
                    float ga = app.elapsedTime * (0.02f + gi * 0.006f) + (float)gi * 0.393f;
                    float bassF = app.audioBass * 2.0f;
                    float gasR = coreSize * (1.4f + sinf(ga * 1.8f + gi) * 0.6f + bassF * 1.2f);
                    float gasY = sinf(ga * 0.5f + gi * 1.3f) * gasR * 0.5f;
                    glm::vec3 gp = n.pos + glm::vec3(cosf(ga)*gasR, gasY, sinf(ga)*gasR);
                    float gs = coreSize * (0.3f + app.audioBass * 0.5f);
                    glm::vec3 gc = nebColors[gi % 8];
                    float ga2 = 0.025f + app.audioWave * 0.025f;
                    app.billboard.draw(gp, glm::vec4(gc, ga2), gs);
                    // Trailing wisps
                    glm::vec3 gp2 = gp + glm::vec3(sinf(ga)*coreSize*0.4f, coreSize*0.15f, cosf(ga)*coreSize*0.3f);
                    app.billboard.draw(gp2, glm::vec4(gc * 0.6f, ga2 * 0.5f), gs * 0.6f);
                }
                
                // GRAVITY LOOPS -- figure-8 / loop patterns driven by audio
                glBindTexture(GL_TEXTURE_2D, app.texAtmosphere);
                for (int li = 0; li < 4; li++) {
                    float loopPhase = app.elapsedTime * 0.15f + (float)li * 1.571f;
                    float loopR = coreSize * (2.5f + app.audioBass * 2.0f + li * 0.5f);
                    int loopSegs = 40;
                    std::vector<float> loopVerts;
                    for (int s = 0; s <= loopSegs; s++) {
                        float t = (float)s / (float)loopSegs * 2.0f * (float)M_PI;
                        // Figure-8 / lemniscate-like path
                        float r = loopR * (0.8f + 0.3f * sinf(t * 2.0f + loopPhase));
                        float x = cosf(t + loopPhase) * r;
                        float y = sinf(t * 2.0f + loopPhase * 0.7f) * loopR * 0.3f;
                        float z = sinf(t + loopPhase) * r;
                        loopVerts.push_back(n.pos.x + x);
                        loopVerts.push_back(n.pos.y + y);
                        loopVerts.push_back(n.pos.z + z);
                    }
                    GLuint lvao, lvbo;
                    glGenVertexArrays(1, &lvao); glGenBuffers(1, &lvbo);
                    glBindVertexArray(lvao); glBindBuffer(GL_ARRAY_BUFFER, lvbo);
                    glBufferData(GL_ARRAY_BUFFER, loopVerts.size()*sizeof(float), loopVerts.data(), GL_STREAM_DRAW);
                    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), 0);
                    glEnableVertexAttribArray(0);
                    app.ringShader.use();
                    app.ringShader.setMat4("uView", glm::value_ptr(view));
                    app.ringShader.setMat4("uProjection", glm::value_ptr(proj));
                    glm::mat4 id(1.0f);
                    app.ringShader.setMat4("uModel", glm::value_ptr(id));
                    glm::vec3 lc = nebColors[li * 2];
                    float la = 0.06f + app.audioWave * 0.04f;
                    app.ringShader.setVec4("uColor", lc.r, lc.g, lc.b, la);
                    glLineWidth(1.5f);
                    glDrawArrays(GL_LINE_STRIP, 0, loopSegs + 1);
                    glLineWidth(1.0f);
                    glDeleteBuffers(1, &lvbo); glDeleteVertexArrays(1, &lvao);
                    
                    // Restore billboard shader for next effects
                    app.billboardShader.use();
                    app.billboardShader.setMat4("uView", glm::value_ptr(view));
                    app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
                    glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
                }
                
                // AUDIO WAVELENGTH RINGS -- expanding rings with frequency
                glBindTexture(GL_TEXTURE_2D, app.texAtmosphere);
                for (int w = 0; w < 5; w++) {
                    float wt = fmodf(app.elapsedTime * 0.4f + (float)w * 0.6f, 3.0f) / 3.0f;
                    float wr = coreSize * (1.5f + wt * 20.0f * (0.5f + app.audioBass * 0.5f));
                    float wa = (1.0f - wt) * 0.03f * (0.3f + app.audioWave * 0.7f);
                    glm::vec3 wc = nebColors[(w + 3) % 8];
                    app.billboard.draw(n.pos, glm::vec4(wc, wa), wr);
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
                int cloudIdx = albumHash % 5; // pick one of the 5 cloud textures
                // Use planetClouds textures
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glBindTexture(GL_TEXTURE_2D, app.texSurface); // fallback for now
                glm::mat4 cm = glm::translate(glm::mat4(1.0f), apos);
                cm = glm::rotate(cm, app.elapsedTime * 0.08f + (float)ai * 2.0f,
                    glm::vec3(tiltX * 0.5f, 1.0f, tiltZ * 0.7f));
                cm = glm::scale(cm, glm::vec3(o.planetSize * 1.02f));
                app.planetShader.setMat4("uModel", glm::value_ptr(cm));
                app.planetShader.setVec3("uColor", 0.5f, 0.5f, 0.55f);
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

void render(App& app) {
    // Render directly to screen -- no bloom (bloom was causing whiteout)
    // The original Planetary used additive-blended sprites for glows,
    // not screen-space bloom. The additive glows on a dark background
    // create the luminous look naturally.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, app.screenW, app.screenH);
    glClearColor(0.0f, 0.0f, 0.005f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderScene(app);
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
        ImVec2 textSize = ImGui::CalcTextSize(name);
        ImVec2 pos(sp.x - textSize.x * 0.5f, sp.y - textSize.y - 4);

        // Shadow for readability
        dl->AddText(ImVec2(pos.x + 1, pos.y + 1), shadow, name);
        dl->AddText(pos, col, name);
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
            ImVec2 ts = ImGui::CalcTextSize(albumUpper.c_str());
            ImVec2 pos(sp.x - ts.x * 0.5f, sp.y - ts.y);
            dl->AddText(ImVec2(pos.x + 1, pos.y + 1), shadow, albumUpper.c_str());
            dl->AddText(pos, col, albumUpper.c_str());

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
                    ImVec2 tts = ImGui::CalcTextSize(trackUpper.c_str());
                    ImVec2 tp(msp.x - tts.x * 0.5f, msp.y - tts.y);
                    dl->AddText(ImVec2(tp.x + 1, tp.y + 1), ts2, trackUpper.c_str());
                    dl->AddText(tp, tc, trackUpper.c_str());
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

    // Search with clickable results list
    if (app.artistNodes.size() > 0) {
        static char searchBuf[256] = "";
        ImGui::SetNextItemWidth(290);
        ImGui::InputTextWithHint("##search", "Search artists...", searchBuf, sizeof(searchBuf));
        app.searchQuery = searchBuf;

        // Show clickable search results when searching
        if (strlen(searchBuf) > 1) {
            std::string q = searchBuf;
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            int shown = 0;
            for (int i = 0; i < (int)app.artistNodes.size() && shown < 15; i++) {
                std::string lower = app.artistNodes[i].name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                if (lower.find(q) != std::string::npos) {
                    if (ImGui::Selectable(app.artistNodes[i].name.c_str(), false, 0, ImVec2(0, 20))) {
                        // Click to navigate to this artist
                        if (app.selectedArtist >= 0) app.artistNodes[app.selectedArtist].isSelected = false;
                        app.selectedArtist = i;
                        app.selectedAlbum = -1;
                        app.artistNodes[i].isSelected = true;
                        app.currentLevel = G_ARTIST_LEVEL;
                        app.camera.autoRotate = false;
                        app.camera.flyTo(app.artistNodes[i].pos, app.artistNodes[i].idealCameraDist);
                        searchBuf[0] = '\0'; // Clear search after selection
                        app.searchQuery = "";
                    }
                    shown++;
                }
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
        // Filter by search
        if (!app.searchQuery.empty()) {
            std::string lower = n.name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            std::string q = app.searchQuery;
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            if (lower.find(q) == std::string::npos) continue;
        }
        glm::vec4 clip = proj * view * glm::vec4(n.pos, 1);
        if (clip.w <= 0) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float sx = (ndc.x * 0.5f + 0.5f) * app.screenW;
        float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * app.screenH;
        float d = sqrtf((sx-mx)*(sx-mx) + (sy-my)*(sy-my));
        float hitR = std::max(20.0f, n.glowRadius * 5.0f / std::max(clip.w * 0.1f, 0.1f));
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

        // Test planet
        glm::vec2 sp = worldToScreen(vp, apos, app.screenW, app.screenH);
        float d = sqrtf((sp.x-mx)*(sp.x-mx) + (sp.y-my)*(sp.y-my));
        float hitR = std::max(25.0f, o.planetSize * 80.0f);
        if (d < hitR && d < bestDist) { bestDist = d; result = {ai, -1}; }

        // Test track moons (only for selected album)
        if (ai == app.selectedAlbum) {
            for (int ti = 0; ti < (int)o.tracks.size(); ti++) {
                auto& t = o.tracks[ti];
                float ta = t.angle + app.elapsedTime * t.speed;
                glm::vec3 mp = getMoonPos(apos, t.radius, ta, t.tiltX, t.tiltZ);
                glm::vec2 msp = worldToScreen(vp, mp, app.screenW, app.screenH);
                float md = sqrtf((msp.x-mx)*(msp.x-mx) + (msp.y-my)*(msp.y-my));
                float mhitR = std::max(20.0f, t.size * 120.0f);
                if (md < mhitR && md < bestDist) { bestDist = md; result = {ai, ti}; }
            }
        }
    }
    return result;
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
            }
            break;
        case SDL_DROPFILE: {
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
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
            if (ev.cbutton.button == SDL_CONTROLLER_BUTTON_A) {
                // A button = select/play (like left click)
                // TODO: raycast from screen center
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

    if (!app.musicPath.empty() && fs::is_directory(app.musicPath)) {
        app.scanning = true;
        std::thread([&app]() {
            app.library = scanMusicLibrary(app.musicPath,
                [&](int d, int t) { app.scanProgress = d; app.scanTotal = t; });
            app.libraryLoaded = true; app.scanning = false;
        }).detach();
    }

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

        // Gamepad input (PS5/Xbox controller via SDL2)
        if (app.controller) {
            float lx = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0f;
            float ly = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0f;
            float rx = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_RIGHTX) / 32768.0f;
            float ry = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_RIGHTY) / 32768.0f;
            float lt = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32768.0f;
            float rt = SDL_GameControllerGetAxis(app.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32768.0f;

            // Right stick: orbit camera
            if (fabsf(rx) > 0.1f || fabsf(ry) > 0.1f) {
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
        render(app);
        renderMeteors(app);
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
