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

#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <random>
#include <chrono>
#include <thread>
#include <atomic>

#include "stb_image.h"
#include "shader.h"
#include "camera.h"
#include "music_data.h"

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

    // Album orbit data
    struct AlbumOrbit {
        float radius;
        float angle;
        float speed;
        float planetSize;
        int numTracks;
        std::string name;
        // Track orbits
        struct TrackOrbit {
            float radius;
            float angle;
            float speed;
            float size;
            std::string name;
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
    if (name.length() >= 3) {
        c1 = name[1];
        c2 = name[2];
    }
    int c1Int = std::clamp((int)c1, 32, 127);
    int c2Int = std::clamp((int)c2, 32, 127);

    int totalCharAscii = (c1Int - 32) + (c2Int - 32);
    float asciiPer = ((float)totalCharAscii / 190.0f) * 5000.0f;

    node.hue = sinf(asciiPer) * 0.35f + 0.35f;
    node.sat = (1.0f - sinf((node.hue + 0.15f) * M_PI)) * 0.75f;

    // HSV to RGB
    auto hsvToRgb = [](float h, float s, float v) -> glm::vec3 {
        float c = v * s;
        float x = c * (1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
        float m = v - c;
        glm::vec3 rgb;
        int hi = (int)(h * 6.0f) % 6;
        switch (hi) {
            case 0: rgb = {c, x, 0}; break;
            case 1: rgb = {x, c, 0}; break;
            case 2: rgb = {0, c, x}; break;
            case 3: rgb = {0, x, c}; break;
            case 4: rgb = {x, 0, c}; break;
            default: rgb = {c, 0, x}; break;
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
// Golden angle spiral with hash-based distance
// ============================================================
void computeArtistPosition(ArtistNode& node, int total) {
    // Original uses hash-based distance and index-based angle
    std::hash<std::string> hasher;
    size_t h = hasher(node.name);
    float hashPer = (float)(h % 9000L) / 90.0f + 10.0f;

    float angle = (float)node.index * 0.618f;
    float x = cosf(angle) * hashPer;
    float z = sinf(angle) * hashPer;
    float y = hashPer * 0.2f - 10.0f;

    node.pos = glm::vec3(x, y, z);
}

// ============================================================
// ALBUM ORBIT LAYOUT - from NodeArtist::setChildOrbitRadii()
// ============================================================
void computeAlbumOrbits(ArtistNode& node, const ArtistData& artistData) {
    node.albumOrbits.clear();

    float orbitOffset = node.radiusInit * 1.25f;
    int albumIdx = 0;

    for (auto& album : artistData.albums) {
        ArtistNode::AlbumOrbit orbit;
        orbit.name = album.name;
        orbit.numTracks = (int)album.tracks.size();

        // Original: amt = max(numTracks * 0.065, 0.2)
        float amt = std::max(orbit.numTracks * 0.065f, 0.2f);
        orbitOffset += amt;
        orbit.radius = orbitOffset;
        orbit.angle = (float)albumIdx * 0.618f * M_PI * 2.0f;
        orbit.speed = 0.15f / sqrtf(std::max(orbit.radius, 0.5f));

        // Planet size from original
        float totalLength = 0;
        for (auto& t : album.tracks) totalLength += t.duration;
        orbit.planetSize = std::max(0.15f, 0.1f + sqrtf((float)orbit.numTracks) * 0.06f);

        // Track moon orbits
        float trackOrbitR = orbit.planetSize * 3.0f;
        for (int ti = 0; ti < (int)album.tracks.size(); ti++) {
            ArtistNode::AlbumOrbit::TrackOrbit to;
            to.name = album.tracks[ti].title;
            to.duration = album.tracks[ti].duration;
            float moonSize = std::max(0.04f, 0.02f + 0.03f * (to.duration / 300.0f));
            trackOrbitR += moonSize * 2.0f;
            to.radius = trackOrbitR;
            to.angle = (float)ti * 2.396f; // golden angle
            to.speed = (2.0f * M_PI) / std::max(to.duration, 30.0f);
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
    int w, h, channels;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data) {
        std::cerr << "[Planetary] Failed to load texture: " << path << std::endl;
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
    std::cout << "[Planetary] Loaded texture: " << path << " (" << w << "x" << h << ")" << std::endl;
    return tex;
}

// ============================================================
// SPHERE MESH - for planets/moons (original: BloomSphere)
// ============================================================
struct SphereMesh {
    GLuint vao = 0, vbo = 0, ebo = 0;
    int indexCount = 0;

    void create(int stacks, int slices) {
        std::vector<float> verts;
        std::vector<unsigned int> indices;

        for (int i = 0; i <= stacks; i++) {
            float phi = M_PI * (float)i / stacks;
            for (int j = 0; j <= slices; j++) {
                float theta = 2.0f * M_PI * (float)j / slices;
                float x = sinf(phi) * cosf(theta);
                float y = cosf(phi);
                float z = sinf(phi) * sinf(theta);
                float u = (float)j / slices;
                float v = (float)i / stacks;
                // position
                verts.push_back(x); verts.push_back(y); verts.push_back(z);
                // normal
                verts.push_back(x); verts.push_back(y); verts.push_back(z);
                // texcoord
                verts.push_back(u); verts.push_back(v);
            }
        }

        for (int i = 0; i < stacks; i++) {
            for (int j = 0; j < slices; j++) {
                int a = i * (slices + 1) + j;
                int b = a + slices + 1;
                indices.push_back(a); indices.push_back(b); indices.push_back(a + 1);
                indices.push_back(b); indices.push_back(b + 1); indices.push_back(a + 1);
            }
        }

        indexCount = (int)indices.size();

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);

        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), indices.data(), GL_STATIC_DRAW);

        // position
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        // normal
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        // texcoord
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);
    }

    void draw() const {
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }
};

// ============================================================
// RING MESH - for orbit rings
// ============================================================
struct RingMesh {
    GLuint vao = 0, vbo = 0;
    int vertCount = 0;

    void create(float radius, int segments) {
        std::vector<float> verts;
        for (int i = 0; i <= segments; i++) {
            float angle = 2.0f * M_PI * (float)i / segments;
            verts.push_back(cosf(angle) * radius);
            verts.push_back(0.0f);
            verts.push_back(sinf(angle) * radius);
        }
        vertCount = segments + 1;

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), 0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    void draw() const {
        glBindVertexArray(vao);
        glDrawArrays(GL_LINE_STRIP, 0, vertCount);
        glBindVertexArray(0);
    }
};

// ============================================================
// BACKGROUND STARS - from original Stars.cpp
// ============================================================
struct BackgroundStars {
    GLuint vao = 0, vbo = 0;
    int count = 0;

    void create(int numStars) {
        count = numStars;
        std::vector<float> data;
        std::mt19937 rng(42);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        std::uniform_real_distribution<float> bright(0.1f, 0.8f);

        for (int i = 0; i < numStars; i++) {
            // Random point on sphere
            float x = dist(rng), y = dist(rng), z = dist(rng);
            float len = sqrtf(x * x + y * y + z * z);
            if (len < 0.001f) continue;
            float r = 300.0f + dist(rng) * 200.0f;
            x = x / len * r; y = y / len * r; z = z / len * r;

            float b = bright(rng);
            float sz = 0.5f + dist(rng) * 0.5f;

            // pos (3), color (4), size (1)
            data.push_back(x); data.push_back(y); data.push_back(z);
            data.push_back(b * 0.8f); data.push_back(b * 0.85f); data.push_back(b); data.push_back(b * 0.6f);
            data.push_back(sz);
        }

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_STATIC_DRAW);

        // pos
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), 0);
        glEnableVertexAttribArray(0);
        // color
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        // size
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(7 * sizeof(float)));
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);
    }

    void draw() const {
        glBindVertexArray(vao);
        glDrawArrays(GL_POINTS, 0, count);
        glBindVertexArray(0);
    }
};

// ============================================================
// BILLBOARD QUAD - for star glows, atmospheres
// ============================================================
struct BillboardQuad {
    GLuint vao = 0, vbo = 0;

    void create() {
        // Single quad: we'll instance/reuse with uniforms
        float verts[] = {
            // pos(3), texcoord(2), color(4), size(1)
            0,0,0, 0,0, 1,1,1,1, 1,
            0,0,0, 1,0, 1,1,1,1, 1,
            0,0,0, 1,1, 1,1,1,1, 1,
            0,0,0, 0,0, 1,1,1,1, 1,
            0,0,0, 1,1, 1,1,1,1, 1,
            0,0,0, 0,1, 1,1,1,1, 1,
        };

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 10 * sizeof(float), 0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(5 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void*)(9 * sizeof(float)));
        glEnableVertexAttribArray(3);

        glBindVertexArray(0);
    }

    void draw(glm::vec3 pos, glm::vec4 color, float size) {
        float verts[] = {
            pos.x,pos.y,pos.z, 0,0, color.r,color.g,color.b,color.a, size,
            pos.x,pos.y,pos.z, 1,0, color.r,color.g,color.b,color.a, size,
            pos.x,pos.y,pos.z, 1,1, color.r,color.g,color.b,color.a, size,
            pos.x,pos.y,pos.z, 0,0, color.r,color.g,color.b,color.a, size,
            pos.x,pos.y,pos.z, 1,1, color.r,color.g,color.b,color.a, size,
            pos.x,pos.y,pos.z, 0,1, color.r,color.g,color.b,color.a, size,
        };

        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBindVertexArray(0);
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

    // Rendering
    Shader starPointShader;
    Shader billboardShader;
    Shader planetShader;
    Shader ringShader;

    GLuint texStarGlow = 0;
    GLuint texAtmosphere = 0;
    GLuint texStar = 0;
    GLuint texSurface = 0;

    BackgroundStars bgStars;
    BillboardQuad billboard;
    SphereMesh sphereHi, sphereMd, sphereLo;
    RingMesh unitRing;

    float elapsedTime = 0;
    bool mouseDown = false;
    int mouseButton = 0;

    // Loading state
    std::atomic<bool> libraryLoaded{false};
    std::atomic<bool> scanning{false};
    std::atomic<int> scanProgress{0};
    std::atomic<int> scanTotal{0};
    std::string musicPath;
};

// ============================================================
// INITIALIZATION
// ============================================================
bool initSDL(App& app) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    app.window = SDL_CreateWindow(
        "Planetary",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        app.screenW, app.screenH,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN
    );

    if (!app.window) {
        std::cerr << "Window creation failed: " << SDL_GetError() << std::endl;
        return false;
    }

    app.glContext = SDL_GL_CreateContext(app.window);
    if (!app.glContext) {
        std::cerr << "GL context failed: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_GL_SetSwapInterval(1); // vsync

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::cerr << "GLEW init failed" << std::endl;
        return false;
    }

    std::cout << "[Planetary] OpenGL " << glGetString(GL_VERSION) << std::endl;
    std::cout << "[Planetary] Renderer: " << glGetString(GL_RENDERER) << std::endl;

    return true;
}

bool initResources(App& app) {
    // Shaders
    if (!app.starPointShader.load("shaders/star_points.vert", "shaders/star_points.frag")) return false;
    if (!app.billboardShader.load("shaders/billboard.vert", "shaders/billboard.frag")) return false;
    if (!app.planetShader.load("shaders/planet.vert", "shaders/planet.frag")) return false;
    if (!app.ringShader.load("shaders/orbit_ring.vert", "shaders/orbit_ring.frag")) return false;

    // Textures from original Planetary
    app.texStarGlow = loadTexture("resources/starGlow.png");
    app.texAtmosphere = loadTexture("resources/atmosphere.png");
    app.texStar = loadTexture("resources/star.png");
    app.texSurface = loadTexture("resources/surfacesLowRes.png");

    // Meshes
    app.bgStars.create(5000);
    app.billboard.create();
    app.sphereHi.create(32, 32);
    app.sphereMd.create(16, 16);
    app.sphereLo.create(8, 8);
    app.unitRing.create(1.0f, 128);

    // OpenGL state
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_MULTISAMPLE);
    glClearColor(0.0f, 0.0f, 0.02f, 1.0f);

    return true;
}

// ============================================================
// BUILD SCENE FROM MUSIC LIBRARY
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
        computeAlbumOrbits(node, app.library.artists[i]);

        node.glowRadius = node.radiusInit * (0.8f + std::min(node.totalTracks / 30.0f, 1.0f) * 1.2f);

        app.artistNodes.push_back(node);
    }

    // Initial camera distance based on galaxy size
    float maxR = 0;
    for (auto& n : app.artistNodes) {
        float r = glm::length(n.pos);
        if (r > maxR) maxR = r;
    }
    app.camera.targetOrbitDist = std::max(maxR * 1.5f, 50.0f);
    app.camera.orbitDist = app.camera.targetOrbitDist;

    std::cout << "[Planetary] Built scene with " << app.artistNodes.size() << " stars" << std::endl;
}

// ============================================================
// RENDERING
// ============================================================
void render(App& app) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glm::mat4 view = app.camera.viewMatrix();
    glm::mat4 proj = app.camera.projMatrix();

    // --- Background stars (point sprites with glow texture) ---
    glDepthMask(GL_FALSE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // Additive blending for stars

    app.starPointShader.use();
    app.starPointShader.setMat4("uView", glm::value_ptr(view));
    app.starPointShader.setMat4("uProjection", glm::value_ptr(proj));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app.texStar);
    app.starPointShader.setInt("uTexture", 0);
    app.bgStars.draw();

    // --- Artist star glows (billboarded sprites) ---
    // Original: drawStarGlow() - 15x radius billboard with glow texture
    app.billboardShader.use();
    app.billboardShader.setMat4("uView", glm::value_ptr(view));
    app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
    app.billboardShader.setVec2("uScreenSize", (float)app.screenW, (float)app.screenH);

    glBindTexture(GL_TEXTURE_2D, app.texStarGlow);
    app.billboardShader.setInt("uTexture", 0);

    for (auto& node : app.artistNodes) {
        float pulse = 1.0f + sinf(app.elapsedTime * 1.5f + (float)node.index) * 0.08f;
        float glowSize = node.glowRadius * 15.0f * pulse;
        float alpha = 0.5f;
        if (node.isSelected) {
            alpha = 0.7f;
            glowSize *= 1.3f;
        }

        app.billboard.draw(node.pos,
            glm::vec4(node.glowColor, alpha),
            glowSize);
    }

    glDepthMask(GL_TRUE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // --- Artist star cores (small lit spheres) ---
    // Original: drawPlanet() - 0.16x radius sphere
    app.planetShader.use();
    app.planetShader.setMat4("uView", glm::value_ptr(view));
    app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
    app.planetShader.setVec3("uLightPos", 0.0f, 50.0f, 0.0f);

    glBindTexture(GL_TEXTURE_2D, app.texSurface);
    app.planetShader.setInt("uTexture", 0);

    for (auto& node : app.artistNodes) {
        float coreSize = node.radius * 0.16f;
        glm::mat4 model = glm::translate(glm::mat4(1.0f), node.pos);
        model = glm::rotate(model, app.elapsedTime * 0.5f, glm::vec3(0, 1, 0));
        model = glm::scale(model, glm::vec3(coreSize));

        app.planetShader.setMat4("uModel", glm::value_ptr(model));
        app.planetShader.setVec3("uColor", node.color.r, node.color.g, node.color.b);
        app.planetShader.setVec3("uEmissive", node.color.r, node.color.g, node.color.b);
        app.planetShader.setFloat("uEmissiveStrength", 0.3f);

        app.sphereLo.draw();
    }

    // --- Selected artist: album planets + orbit rings ---
    if (app.selectedArtist >= 0 && app.selectedArtist < (int)app.artistNodes.size()) {
        auto& star = app.artistNodes[app.selectedArtist];

        // Orbit rings
        app.ringShader.use();
        app.ringShader.setMat4("uView", glm::value_ptr(view));
        app.ringShader.setMat4("uProjection", glm::value_ptr(proj));

        for (auto& orbit : star.albumOrbits) {
            glm::mat4 model = glm::translate(glm::mat4(1.0f), star.pos);
            model = glm::scale(model, glm::vec3(orbit.radius));
            app.ringShader.setMat4("uModel", glm::value_ptr(model));
            app.ringShader.setVec4("uColor", BRIGHT_BLUE.r, BRIGHT_BLUE.g, BRIGHT_BLUE.b, 0.15f);
            app.unitRing.draw();
        }

        // Album planets
        app.planetShader.use();
        glBindTexture(GL_TEXTURE_2D, app.texSurface);

        for (auto& orbit : star.albumOrbits) {
            float angle = orbit.angle + app.elapsedTime * orbit.speed;
            glm::vec3 albumPos = star.pos + glm::vec3(
                cosf(angle) * orbit.radius,
                0.0f,
                sinf(angle) * orbit.radius
            );

            glm::mat4 model = glm::translate(glm::mat4(1.0f), albumPos);
            model = glm::rotate(model, app.elapsedTime * 0.3f, glm::vec3(0, 1, 0));
            model = glm::scale(model, glm::vec3(orbit.planetSize));

            float planetHue = star.hue + 0.015f;
            glm::vec3 planetColor = star.color * 0.9f;

            app.planetShader.setMat4("uModel", glm::value_ptr(model));
            app.planetShader.setVec3("uColor", planetColor.r, planetColor.g, planetColor.b);
            app.planetShader.setVec3("uLightPos", star.pos.x, star.pos.y + 2.0f, star.pos.z);
            app.planetShader.setVec3("uEmissive", planetColor.r, planetColor.g, planetColor.b);
            app.planetShader.setFloat("uEmissiveStrength", 0.15f);

            app.sphereMd.draw();

            // Atmosphere glow
            glDepthMask(GL_FALSE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            app.billboardShader.use();
            app.billboardShader.setMat4("uView", glm::value_ptr(view));
            app.billboardShader.setMat4("uProjection", glm::value_ptr(proj));
            glBindTexture(GL_TEXTURE_2D, app.texAtmosphere);
            app.billboard.draw(albumPos,
                glm::vec4(star.glowColor, 0.25f),
                orbit.planetSize * 4.84f);
            glDepthMask(GL_TRUE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            // Track moons (only for selected album)
            if (app.selectedAlbum >= 0) {
                int albumIdx = 0;
                for (auto& ao : star.albumOrbits) {
                    if (albumIdx == app.selectedAlbum) {
                        // Draw track orbit rings
                        app.ringShader.use();
                        app.ringShader.setMat4("uView", glm::value_ptr(view));
                        app.ringShader.setMat4("uProjection", glm::value_ptr(proj));

                        for (auto& track : ao.tracks) {
                            glm::mat4 rm = glm::translate(glm::mat4(1.0f), albumPos);
                            rm = glm::scale(rm, glm::vec3(track.radius));
                            app.ringShader.setMat4("uModel", glm::value_ptr(rm));
                            app.ringShader.setVec4("uColor", BRIGHT_BLUE.r, BRIGHT_BLUE.g, BRIGHT_BLUE.b, 0.08f);
                            app.unitRing.draw();
                        }

                        // Draw track moons
                        app.planetShader.use();
                        glBindTexture(GL_TEXTURE_2D, app.texSurface);
                        for (auto& track : ao.tracks) {
                            float ta = track.angle + app.elapsedTime * track.speed;
                            glm::vec3 moonPos = albumPos + glm::vec3(
                                cosf(ta) * track.radius, 0, sinf(ta) * track.radius);

                            glm::mat4 mm = glm::translate(glm::mat4(1.0f), moonPos);
                            mm = glm::scale(mm, glm::vec3(track.size));
                            app.planetShader.setMat4("uModel", glm::value_ptr(mm));
                            app.planetShader.setVec3("uColor", star.color.r, star.color.g, star.color.b);
                            app.planetShader.setVec3("uEmissive", star.color.r, star.color.g, star.color.b);
                            app.planetShader.setFloat("uEmissiveStrength", 0.2f);
                            app.sphereLo.draw();
                        }
                        break;
                    }
                    albumIdx++;
                }
            }

            // Restore planet shader for next planet
            app.planetShader.use();
            app.planetShader.setMat4("uView", glm::value_ptr(view));
            app.planetShader.setMat4("uProjection", glm::value_ptr(proj));
            glBindTexture(GL_TEXTURE_2D, app.texSurface);
        }
    }

    SDL_GL_SwapWindow(app.window);
}

// ============================================================
// HIT TESTING - find which star was clicked
// ============================================================
int hitTestStar(App& app, int mx, int my) {
    glm::mat4 view = app.camera.viewMatrix();
    glm::mat4 proj = app.camera.projMatrix();

    float bestDist = 999999.0f;
    int bestIdx = -1;

    for (int i = 0; i < (int)app.artistNodes.size(); i++) {
        auto& node = app.artistNodes[i];
        glm::vec4 clip = proj * view * glm::vec4(node.pos, 1.0f);
        if (clip.w <= 0) continue;
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        float sx = (ndc.x * 0.5f + 0.5f) * app.screenW;
        float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * app.screenH;

        float dx = sx - mx;
        float dy = sy - my;
        float screenDist = sqrtf(dx * dx + dy * dy);

        // Hit radius based on glow size (proportional to screen projection)
        float hitR = std::max(20.0f, node.glowRadius * 5.0f / std::max(clip.w * 0.1f, 0.1f));

        if (screenDist < hitR && screenDist < bestDist) {
            bestDist = screenDist;
            bestIdx = i;
        }
    }

    return bestIdx;
}

// ============================================================
// EVENT HANDLING
// ============================================================
void handleEvents(App& app) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_QUIT:
            app.running = false;
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                app.screenW = event.window.data1;
                app.screenH = event.window.data2;
                app.camera.aspect = (float)app.screenW / (float)app.screenH;
                glViewport(0, 0, app.screenW, app.screenH);
            }
            break;

        case SDL_MOUSEBUTTONDOWN:
            app.mouseDown = true;
            app.mouseButton = event.button.button;

            if (event.button.button == SDL_BUTTON_LEFT) {
                int hit = hitTestStar(app, event.button.x, event.button.y);
                if (hit >= 0) {
                    // Deselect previous
                    if (app.selectedArtist >= 0)
                        app.artistNodes[app.selectedArtist].isSelected = false;

                    if (hit == app.selectedArtist) {
                        // Clicked same star - deselect
                        app.selectedArtist = -1;
                        app.selectedAlbum = -1;
                        app.currentLevel = G_ALPHA_LEVEL;
                        app.camera.autoRotate = true;

                        // Zoom back out
                        float maxR = 0;
                        for (auto& n : app.artistNodes)
                            maxR = std::max(maxR, glm::length(n.pos));
                        app.camera.flyTo(glm::vec3(0), maxR * 1.5f);
                    } else {
                        // Select new star
                        app.selectedArtist = hit;
                        app.artistNodes[hit].isSelected = true;
                        app.currentLevel = G_ARTIST_LEVEL;
                        app.camera.autoRotate = false;

                        // Fly to star
                        auto& node = app.artistNodes[hit];
                        app.camera.flyTo(node.pos, node.idealCameraDist);
                    }
                }
            }
            break;

        case SDL_MOUSEBUTTONUP:
            app.mouseDown = false;
            break;

        case SDL_MOUSEMOTION:
            if (app.mouseDown && app.mouseButton == SDL_BUTTON_RIGHT) {
                app.camera.onMouseDrag((float)event.motion.xrel, (float)event.motion.yrel);
            }
            break;

        case SDL_MOUSEWHEEL:
            app.camera.onMouseScroll((float)event.wheel.y);
            break;

        case SDL_KEYDOWN:
            if (event.key.keysym.sym == SDLK_ESCAPE) {
                if (app.selectedArtist >= 0) {
                    app.artistNodes[app.selectedArtist].isSelected = false;
                    app.selectedArtist = -1;
                    app.selectedAlbum = -1;
                    app.currentLevel = G_ALPHA_LEVEL;
                    app.camera.autoRotate = true;
                    float maxR = 0;
                    for (auto& n : app.artistNodes)
                        maxR = std::max(maxR, glm::length(n.pos));
                    app.camera.flyTo(glm::vec3(0), maxR * 1.5f);
                } else {
                    app.running = false;
                }
            }
            break;

        case SDL_DROPFILE: {
            // Drag and drop a folder to scan
            char* path = event.drop.file;
            if (fs::is_directory(path)) {
                app.musicPath = path;
                app.scanning = true;
                std::thread([&app]() {
                    app.library = scanMusicLibrary(app.musicPath,
                        [&](int done, int total) {
                            app.scanProgress = done;
                            app.scanTotal = total;
                        });
                    app.libraryLoaded = true;
                    app.scanning = false;
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
int main(int argc, char* argv[]) {
    App app;

    if (!initSDL(app)) return 1;
    if (!initResources(app)) return 1;

    // If a path was passed as argument, scan it
    if (argc > 1) {
        app.musicPath = argv[1];
        if (fs::is_directory(app.musicPath)) {
            app.scanning = true;
            std::thread([&app]() {
                app.library = scanMusicLibrary(app.musicPath,
                    [&](int done, int total) {
                        app.scanProgress = done;
                        app.scanTotal = total;
                    });
                app.libraryLoaded = true;
                app.scanning = false;
            }).detach();
        }
    }

    auto prevTime = std::chrono::high_resolution_clock::now();

    while (app.running) {
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        app.elapsedTime += dt;

        handleEvents(app);

        // Check if library finished loading
        if (app.libraryLoaded.exchange(false)) {
            buildScene(app);
        }

        app.camera.update(dt);
        render(app);
    }

    SDL_GL_DeleteContext(app.glContext);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
