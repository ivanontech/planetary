cmake_minimum_required(VERSION 3.22)
project(planetary_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── SDL2 2.28.5 (bundled) ─────────────────────────────────────────────────
# Fixes for SDL2 + Android NDK r25 cross-compilation issues:
set(SDL_TEST          OFF CACHE BOOL "" FORCE)
set(SDL_STATIC        OFF CACHE BOOL "" FORCE)
set(SDL_SHARED        ON  CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
# Tell SDL2 alloca comes from <alloca.h>, not <malloc.h>
set(HAVE_ALLOCA_H     ON  CACHE BOOL "" FORCE)
set(HAVE_ALLOCA       ON  CACHE BOOL "" FORCE)
# Don't use -Wl,--no-undefined during configure tests
set(HAVE_NO_UNDEFINED OFF CACHE BOOL "" FORCE)

add_subdirectory(SDL EXCLUDE_FROM_ALL)

# ── GLM (header-only, fetched at configure time) ──────────────────────────
include(FetchContent)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glm)

# ── Planetary native library ──────────────────────────────────────────────
set(PLANETARY_SRC
    ../../../../../src/main.cpp
    ../../../../../src/stb_image_impl.cpp
    ../../../../../src/miniaudio_impl.cpp
    ../../../../../src/imgui/imgui.cpp
    ../../../../../src/imgui/imgui_draw.cpp
    ../../../../../src/imgui/imgui_tables.cpp
    ../../../../../src/imgui/imgui_widgets.cpp
    ../../../../../src/imgui/imgui_impl_sdl2.cpp
    ../../../../../src/imgui/imgui_impl_opengl3.cpp
)

add_library(main SHARED ${PLANETARY_SRC})

target_include_directories(main PRIVATE
    ../../../../../src
    ../../../../../src/imgui
    SDL/include
)

target_compile_definitions(main PRIVATE
    __ANDROID__
    ANDROID
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    PLANETARY_NO_TAGLIB
)

find_library(log-lib    log)
find_library(android-lib android)
find_library(gles3-lib  GLESv3)
find_library(egl-lib    EGL)

target_link_libraries(main
    SDL2::SDL2
    glm::glm
    ${log-lib}
    ${android-lib}
    ${gles3-lib}
    ${egl-lib}
)
