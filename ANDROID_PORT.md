# Planetary → Android TV (NVIDIA Shield) Port Guide

## Architecture: Client-Server Model

**⚠️ Shield TV cannot store 9,000+ songs locally.** Planetary on Shield must work as a **streaming client** (like Jellyfin/Plex):

### Music Server (runs on Mac Studio)
- **Option A: Navidrome** — lightweight, self-hosted, Subsonic-compatible API, Go binary
  - `brew install navidrome` → point at `~/Desktop/Music`
  - REST API for browse/search/stream: `/rest/search3`, `/rest/stream?id=xxx`
  - Subsonic API is well-documented, many clients support it
- **Option B: Custom HTTP server** — simple Python/Node server
  - Serves `~/Desktop/Music` directory
  - JSON index endpoint: `GET /api/library` → returns all tracks with metadata
  - Stream endpoint: `GET /api/stream/{trackId}` → returns audio file
  - Lightweight, no dependencies, we control the API

### Planetary Shield Client
- Replaces local filesystem music scanning with HTTP API calls
- Browse/search library over LAN
- Stream audio tracks on demand (miniaudio supports HTTP streaming or we buffer)
- All rendering stays local on Shield's Tegra X1 GPU

### Why This Is Better
- Works on ANY device (Shield, phone, tablet, desktop) — same library
- No storage issues on Shield (16GB tube model)
- Library stays in one place (Mac Studio)
- Future: could add remote access for outside-home use

---

## Technical: SDL2 Android NDK Build

### Prerequisites
- Android SDK (API 21+ for Shield TV)
- Android NDK r25+ (for C++17 support)
- SDL2 source (has Android project template in `android-project/`)

### Key Changes from Desktop

#### 1. OpenGL → OpenGL ES
- Desktop: OpenGL 3.3 Core + GLEW
- Android: OpenGL ES 3.0/3.1 (Tegra X1 supports ES 3.1)
- Replace `#include <GL/glew.h>` with `<GLES3/gl3.h>`
- Remove `glewInit()` call
- Shaders: `#version 330 core` → `#version 300 es` + `precision mediump float;`
- ES-compatible shaders already created at `shaders/es/`

#### 2. SDL2 Context Setup
```cpp
// Desktop (current):
SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

// Android:
SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
```

#### 3. ImGui Init
```cpp
// Desktop: ImGui_ImplOpenGL3_Init("#version 330");
// Android: ImGui_ImplOpenGL3_Init("#version 300 es");
```

#### 4. Music Library Access
Replace filesystem scanning with HTTP client:
```cpp
// Instead of scanning ~/Desktop/Music:
// 1. HTTP GET to music server for library index
// 2. Display in ImGui browser
// 3. HTTP GET to stream selected track
// 4. Feed audio data to miniaudio
```

#### 5. Asset/Shader Paths
- Android assets go in `app/src/main/assets/`
- Use `SDL_GetBasePath()` or Android asset manager
- Copy shaders from `shaders/es/` to assets

### main.cpp Change Points (DO NOT EDIT — reference only)
- Line ~1: `#include <GL/glew.h>` → ifdef for GLES
- Lines ~730-732: SDL GL context attributes → ES profile
- Line ~751: Remove `glewInit()` on Android
- Line ~795: ImGui version string → "300 es"
- Lines ~870-883: Shader paths → select `shaders/es/`
- Lines ~2679, ~2962: Music scan → HTTP API calls

### ADB Sideload to Shield
```bash
# Enable developer mode on Shield: Settings → Device Preferences → About → Build (click 7x)
# Enable network debugging: Settings → Device Preferences → Developer Options → Network debugging

# Install APK
adb connect 10.0.0.37:5555
adb install app/build/outputs/apk/debug/app-debug.apk

# View logs
adb logcat | grep planetary
```

**Note:** Mullvad VPN is active but LAN sharing is ON — ADB over local network works fine.

---

## Build Scaffold
Android build files created at `planetary-src/android/`:
- `settings.gradle`, `build.gradle`, `gradle.properties`
- `app/build.gradle` — NDK CMake integration
- `app/src/main/AndroidManifest.xml` — Android TV launcher + leanback
- `app/src/main/cpp/CMakeLists.txt` — native build config

## Shader ES Versions
ES-compatible shaders at `planetary-src/shaders/es/` — all converted from GLSL 330 to 300 es.

---

*Last updated: 2026-02-24. Port researched by Elon sub-agent.*
