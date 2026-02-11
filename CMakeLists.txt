cmake_minimum_required(VERSION 3.16)
project(Planetary VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(GLEW REQUIRED glew)
pkg_check_modules(TAGLIB REQUIRED taglib)

find_package(OpenGL REQUIRED)

# Sources
file(GLOB SOURCES src/*.cpp)

add_executable(planetary ${SOURCES})

target_include_directories(planetary PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
    src/
)

target_link_libraries(planetary
    ${SDL2_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${TAGLIB_LIBRARIES}
    ${OPENGL_LIBRARIES}
    -lm
)

target_compile_options(planetary PRIVATE ${SDL2_CFLAGS_OTHER})

# Copy resources to build directory
file(COPY resources DESTINATION ${CMAKE_BINARY_DIR})
file(COPY shaders DESTINATION ${CMAKE_BINARY_DIR})
