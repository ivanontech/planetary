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
void computeArtistPosition(ArtistNode& node, int total) {
    std::hash<std::string> hasher;
    size_t h = hasher(node.name);
    float hashPer = (float)(h % 9000L) / 90.0f + 10.0f;
    float angle = (float)node.index * 0.618f;
    node.pos = glm::vec3(cosf(angle) * hashPer, hashPer * 0.2f - 10.0f, sinf(angle) * hashPer);
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
        orbit.speed = 0.15f / sqrtf(std::max(orbit.radius, 0.5f));
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
            to.speed = (2.0f * (float)M_PI) / std::max(to.duration, 30.0f);
            to.size = moonSize;
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
    std::string searchQuery;

    // Rendering
    Shader starPointShader, billboardShader, planetShader, ringShader;
    Shader bloomBrightShader, bloomBlurShader, bloomCompositeShader;
    GLuint texStarGlow=0, texAtmosphere=0, texStar=0, texSurface=0;
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

    float elapsedTime = 0;
    bool mouseDown = false;
    int mouseButton = 0;
    bool imguiWantsMouse = false;

    // Loading state
    std::atomic<bool> libraryLoaded{false};
    std::atomic<bool> scanning{false};
    std::atomic<int> scanProgress{0};
    std::atomic<int> scanTotal{0};
    std::string musicPath;
    std::string statusMsg;
};

// ============================================================
// INIT
// ============================================================
bool initSDL(App& app) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
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
    if (!app.bloomBrightShader.load(resolvePath("shaders/fullscreen.vert"), resolvePath("shaders/bloom_bright.frag"))) return false;
    if (!app.bloomBlurShader.load(resolvePath("shaders/fullscreen.vert"), resolvePath("shaders/bloom_blur.frag"))) return false;
    if (!app.bloomCompositeShader.load(resolvePath("shaders/fullscreen.vert"), resolvePath("shaders/bloom_composite.frag"))) return false;

    app.texStarGlow = loadTexture("resources/starGlow.png");
    app.texAtmosphere = loadTexture("resources/atmosphere.png");
    app.texStar = loadTexture("resources/star.png");
    app.texSurface = loadTexture("resources/surfacesLowRes.png");

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
        computeArtistPosition(node, total);
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
}

// ============================================================
// RENDERING
// ============================================================
// Project 3D position to screen coords for text labels
glm::vec2 worldToScreen(const glm::mat4& vp, glm::vec3 pos, int w, int h) {
    glm::vec4 clip = vp * glm::vec4(pos, 1.0f);
    if (clip.w <= 0.01f) return glm::vec2(-1000, -1000);
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * w, (1.0f - (ndc.y * 0.5f + 0.5f)) * h);
}

void renderScene(App& app) {
    glm::mat4 view = app.camera.viewMatrix();
    glm::mat4 proj = app.camera.projMatrix();

    // --- Background stars ---
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    app.starPointShader.use();
    app.starPointShader.setMat4("uView", glm::value_ptr(view));
    app.starPointShader.setMat4("uProjection", glm::value_ptr(proj));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app.texStar);
    app.starPointShader.setInt("uTexture", 0);
    app.bgStars.draw();

    // --- Star glows (additive billboards) ---
    app.billboardShader.use();
    app.billboardShader.setMat4("uView", glm::value_ptr(view));
    app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
    glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
    float glowAlphaScale = std::min(1.0f, 30.0f / std::max((float)app.artistNodes.size(), 1.0f));

    for (auto& n : app.artistNodes) {
        float distToCam = glm::length(n.pos - app.camera.position);
        float distFade = std::clamp(1.0f - (distToCam / 300.0f), 0.0f, 1.0f);
        if (distFade < 0.01f && !n.isSelected) continue;
        float pulse = 1.0f + sinf(app.elapsedTime * 1.5f + (float)n.index) * 0.08f;
        float sz = n.glowRadius * 10.0f * pulse * (n.isSelected ? 1.5f : 1.0f);
        float alpha = (n.isSelected ? 0.6f : 0.12f * glowAlphaScale) * distFade;
        app.billboard.draw(n.pos, glm::vec4(n.glowColor, alpha), sz);
    }

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // --- Star cores (emissive spheres) ---
    app.planetShader.use();
    app.planetShader.setMat4("uView", glm::value_ptr(view));
    app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
    app.planetShader.setVec3("uLightPos", 0, 50, 0);
    glBindTexture(GL_TEXTURE_2D, app.texSurface);
    for (auto& n : app.artistNodes) {
        float cs = n.radius * 0.16f;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), n.pos);
        m = glm::rotate(m, app.elapsedTime * 0.5f, glm::vec3(0,1,0));
        m = glm::scale(m, glm::vec3(cs));
        app.planetShader.setMat4("uModel", glm::value_ptr(m));
        // Bright white-hot core with colored outer
        glm::vec3 coreColor = glm::mix(n.color, glm::vec3(1.0f), 0.5f);
        app.planetShader.setVec3("uColor", coreColor.r, coreColor.g, coreColor.b);
        app.planetShader.setVec3("uEmissive", n.color.r, n.color.g, n.color.b);
        app.planetShader.setFloat("uEmissiveStrength", 0.6f);
        app.sphereLo.draw();
    }

    // --- Selected artist: orbit rings + album planets ---
    if (app.selectedArtist >= 0 && app.selectedArtist < (int)app.artistNodes.size()) {
        auto& star = app.artistNodes[app.selectedArtist];

        // Orbit rings
        app.ringShader.use();
        app.ringShader.setMat4("uView", glm::value_ptr(view));
        app.ringShader.setMat4("uProjection", glm::value_ptr(proj));
        for (auto& o : star.albumOrbits) {
            glm::mat4 rm = glm::translate(glm::mat4(1.0f), star.pos);
            rm = glm::scale(rm, glm::vec3(o.radius));
            app.ringShader.setMat4("uModel", glm::value_ptr(rm));
            app.ringShader.setVec4("uColor", BRIGHT_BLUE.r, BRIGHT_BLUE.g, BRIGHT_BLUE.b, 0.12f);
            app.unitRing.draw();
        }

        // Album planets
        app.planetShader.use();
        app.planetShader.setMat4("uView", glm::value_ptr(view));
        app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
        glBindTexture(GL_TEXTURE_2D, app.texSurface);

        for (int ai = 0; ai < (int)star.albumOrbits.size(); ai++) {
            auto& o = star.albumOrbits[ai];
            float angle = o.angle + app.elapsedTime * o.speed;
            glm::vec3 apos = star.pos + glm::vec3(cosf(angle)*o.radius, 0, sinf(angle)*o.radius);

            glm::mat4 pm = glm::translate(glm::mat4(1.0f), apos);
            pm = glm::rotate(pm, app.elapsedTime * 0.3f, glm::vec3(0,1,0));
            pm = glm::scale(pm, glm::vec3(o.planetSize));
            app.planetShader.setMat4("uModel", glm::value_ptr(pm));
            app.planetShader.setVec3("uColor", star.color.r*0.9f, star.color.g*0.9f, star.color.b*0.9f);
            app.planetShader.setVec3("uLightPos", star.pos.x, star.pos.y+2, star.pos.z);
            app.planetShader.setVec3("uEmissive", star.color.r, star.color.g, star.color.b);
            app.planetShader.setFloat("uEmissiveStrength", ai == app.selectedAlbum ? 0.5f : 0.15f);
            app.sphereHi.draw();

            // Atmosphere glow
            glDepthMask(GL_FALSE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            app.billboardShader.use();
            app.billboardShader.setMat4("uView", glm::value_ptr(view));
            app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
            glBindTexture(GL_TEXTURE_2D, app.texAtmosphere);
            app.billboard.draw(apos, glm::vec4(star.glowColor * 0.5f, 0.25f), o.planetSize * 4.84f);
            glDepthMask(GL_TRUE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Track moons for selected album
            if (ai == app.selectedAlbum) {
                app.ringShader.use();
                app.ringShader.setMat4("uView", glm::value_ptr(view));
                app.ringShader.setMat4("uProjection", glm::value_ptr(proj));
                for (auto& t : o.tracks) {
                    glm::mat4 trm = glm::translate(glm::mat4(1.0f), apos);
                    trm = glm::scale(trm, glm::vec3(t.radius));
                    app.ringShader.setMat4("uModel", glm::value_ptr(trm));
                    app.ringShader.setVec4("uColor", BRIGHT_BLUE.r, BRIGHT_BLUE.g, BRIGHT_BLUE.b, 0.06f);
                    app.unitRing.draw();
                }
                app.planetShader.use();
                app.planetShader.setMat4("uView", glm::value_ptr(view));
                app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
                glBindTexture(GL_TEXTURE_2D, app.texSurface);
                for (auto& t : o.tracks) {
                    float ta = t.angle + app.elapsedTime * t.speed;
                    glm::vec3 mp = apos + glm::vec3(cosf(ta)*t.radius, 0, sinf(ta)*t.radius);
                    glm::mat4 mm = glm::translate(glm::mat4(1.0f), mp);
                    mm = glm::scale(mm, glm::vec3(t.size));
                    app.planetShader.setMat4("uModel", glm::value_ptr(mm));
                    app.planetShader.setVec3("uColor", star.color.r, star.color.g, star.color.b);
                    app.planetShader.setVec3("uEmissive", star.color.r, star.color.g, star.color.b);
                    app.planetShader.setFloat("uEmissiveStrength", 0.2f);
                    app.sphereMd.draw();
                }
            }
            app.planetShader.use();
            app.planetShader.setMat4("uView", glm::value_ptr(view));
            app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
            glBindTexture(GL_TEXTURE_2D, app.texSurface);
        }
    }
}

void render(App& app) {
    // Render scene to FBO for bloom
    glBindFramebuffer(GL_FRAMEBUFFER, app.sceneFBO);
    glViewport(0, 0, app.screenW, app.screenH);
    glClearColor(0.0f, 0.0f, 0.005f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    renderScene(app);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // --- BLOOM: Extract bright parts ---
    glBindFramebuffer(GL_FRAMEBUFFER, app.bloomFBO[0]);
    glViewport(0, 0, app.bloomW, app.bloomH);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    app.bloomBrightShader.use();
    app.bloomBrightShader.setFloat("uThreshold", 0.3f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app.sceneColor);
    app.bloomBrightShader.setInt("uScene", 0);
    drawFullscreenQuad(app);

    // --- BLOOM: Gaussian blur (ping-pong, 5 passes) ---
    app.bloomBlurShader.use();
    for (int pass = 0; pass < 10; pass++) {
        bool horiz = (pass % 2 == 0);
        glBindFramebuffer(GL_FRAMEBUFFER, app.bloomFBO[horiz ? 1 : 0]);
        glBindTexture(GL_TEXTURE_2D, app.bloomColor[horiz ? 0 : 1]);
        app.bloomBlurShader.setInt("uImage", 0);
        app.bloomBlurShader.setInt("uHorizontal", horiz ? 1 : 0);
        app.bloomBlurShader.setFloat("uTexelSize", 1.0f / (horiz ? app.bloomW : app.bloomH));
        drawFullscreenQuad(app);
    }

    // --- BLOOM: Composite ---
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, app.screenW, app.screenH);
    glClear(GL_COLOR_BUFFER_BIT);
    app.bloomCompositeShader.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app.sceneColor);
    app.bloomCompositeShader.setInt("uScene", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, app.bloomColor[0]);
    app.bloomCompositeShader.setInt("uBloom", 1);
    app.bloomCompositeShader.setFloat("uBloomStrength", 0.8f);
    drawFullscreenQuad(app);

    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
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
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(n.color.r, n.color.g, n.color.b, alpha * 0.9f));
        ImU32 shadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, alpha * 0.6f));

        const char* name = n.name.c_str();
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

            float alpha = (ai == app.selectedAlbum) ? 1.0f : 0.6f;
            ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(0.7f, 0.85f, 1.0f, alpha));
            ImU32 shadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, alpha * 0.5f));

            ImVec2 ts = ImGui::CalcTextSize(o.name.c_str());
            ImVec2 pos(sp.x - ts.x * 0.5f, sp.y - ts.y);
            dl->AddText(ImVec2(pos.x + 1, pos.y + 1), shadow, o.name.c_str());
            dl->AddText(pos, col, o.name.c_str());

            // Track moon labels for selected album
            if (ai == app.selectedAlbum) {
                for (auto& t : o.tracks) {
                    float ta = t.angle + app.elapsedTime * t.speed;
                    glm::vec3 mp = apos + glm::vec3(cosf(ta)*t.radius, 0, sinf(ta)*t.radius);
                    glm::vec2 msp = worldToScreen(vp, mp + glm::vec3(0, t.size * 2.0f, 0), app.screenW, app.screenH);
                    if (msp.x < 0 || msp.x > app.screenW) continue;

                    ImU32 tc = ImGui::ColorConvertFloat4ToU32(ImVec4(0.6f, 0.7f, 0.8f, 0.7f));
                    ImU32 ts2 = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.4f));
                    ImVec2 tts = ImGui::CalcTextSize(t.name.c_str());
                    ImVec2 tp(msp.x - tts.x * 0.5f, msp.y - tts.y);
                    dl->AddText(ImVec2(tp.x + 1, tp.y + 1), ts2, t.name.c_str());
                    dl->AddText(tp, tc, t.name.c_str());
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

    // Search
    if (app.artistNodes.size() > 0) {
        static char searchBuf[256] = "";
        ImGui::SetNextItemWidth(320);
        ImGui::InputTextWithHint("##search", "Search artists...", searchBuf, sizeof(searchBuf));
        app.searchQuery = searchBuf;
    }

    // Selected artist info
    if (app.selectedArtist >= 0 && app.selectedArtist < (int)app.artistNodes.size()) {
        auto& star = app.artistNodes[app.selectedArtist];
        ImGui::Separator();
        ImGui::TextColored(ImVec4(star.color.r, star.color.g, star.color.b, 1.0f),
            "%s", star.name.c_str());
        ImGui::TextColored(ImVec4(0.5f, 0.6f, 0.7f, 0.8f),
            "%d albums, %d tracks", (int)star.albumOrbits.size(), star.totalTracks);

        // Album list
        for (int i = 0; i < (int)star.albumOrbits.size(); i++) {
            auto& album = star.albumOrbits[i];
            bool selected = (i == app.selectedAlbum);
            if (ImGui::Selectable(album.name.c_str(), selected)) {
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

        // Track info
        ImGui::BeginGroup();
        ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", app.audio.currentTrackName.c_str());
        ImGui::TextColored(ImVec4(0.5f, 0.6f, 0.7f, 0.8f), "%s - %s",
            app.audio.currentArtist.c_str(), app.audio.currentAlbum.c_str());
        ImGui::EndGroup();

        ImGui::SameLine(300);

        // Progress bar
        float prog = app.audio.progress();
        float ct = app.audio.currentTime();
        int cm = (int)ct / 60, cs = (int)ct % 60;
        int dm = (int)app.audio.duration / 60, ds = (int)app.audio.duration % 60;
        char timeStr[64];
        snprintf(timeStr, sizeof(timeStr), "%d:%02d / %d:%02d", cm, cs, dm, ds);

        ImGui::SetNextItemWidth((float)app.screenW - 500);
        ImGui::ProgressBar(prog, ImVec2(-1, 14), timeStr);

        // Volume
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
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
                app.mouseDown = true; app.mouseButton = ev.button.button;
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    int hit = hitTestStar(app, ev.button.x, ev.button.y);
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
            break;
        case SDL_MOUSEBUTTONUP: app.mouseDown = false; break;
        case SDL_MOUSEMOTION:
            if (app.mouseDown && !app.imguiWantsMouse && app.mouseButton == SDL_BUTTON_RIGHT)
                app.camera.onMouseDrag((float)ev.motion.xrel, (float)ev.motion.yrel);
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

    if (argc > 1) {
        app.musicPath = argv[1];
        if (fs::is_directory(app.musicPath)) {
            app.scanning = true;
            std::thread([&app]() {
                app.library = scanMusicLibrary(app.musicPath,
                    [&](int d, int t) { app.scanProgress = d; app.scanTotal = t; });
                app.libraryLoaded = true; app.scanning = false;
            }).detach();
        }
    }

    auto prev = std::chrono::high_resolution_clock::now();
    while (app.running) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prev).count();
        prev = now;
        app.elapsedTime += dt;

        handleEvents(app);
        if (app.libraryLoaded.exchange(false)) buildScene(app);

        // Rebuild bloom FBOs on resize
        if (app.screenW / 2 != app.bloomW || app.screenH / 2 != app.bloomH) {
            setupBloom(app);
        }

        app.camera.update(dt);
        render(app);
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
