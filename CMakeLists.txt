cmake_minimum_required(VERSION 3.16)
project(Planetary VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sources
file(GLOB SOURCES src/*.cpp)
file(GLOB IMGUI_SOURCES src/imgui/*.cpp)
list(APPEND SOURCES ${IMGUI_SOURCES})

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # ===============================================
    # WINDOWS CROSS-COMPILE BUILD
    # ===============================================
    set(WIN_DEPS "${CMAKE_SOURCE_DIR}/win-deps")
    set(SDL2_DIR "${WIN_DEPS}/SDL2-2.30.10/x86_64-w64-mingw32")
    set(GLEW_SRC "${WIN_DEPS}/glew-src")
    set(TAGLIB_DIR "${WIN_DEPS}/taglib-install")

    # Compile GLEW from source (static)
    add_library(glew_static STATIC ${GLEW_SRC}/src/glew.c)
    target_include_directories(glew_static PUBLIC ${GLEW_SRC}/include)
    target_compile_definitions(glew_static PUBLIC GLEW_STATIC)
    target_link_libraries(glew_static opengl32)

    # Main executable
    add_executable(planetary WIN32 ${SOURCES})

    target_compile_definitions(planetary PRIVATE GLEW_STATIC TAGLIB_STATIC)

    target_include_directories(planetary PRIVATE
        ${SDL2_DIR}/include
        ${SDL2_DIR}/include/SDL2
        ${GLEW_SRC}/include
        ${TAGLIB_DIR}/include
        ${TAGLIB_DIR}/include/taglib
        ${WIN_DEPS}/glm
        ${WIN_DEPS}
        src/
    )

    target_compile_options(planetary PRIVATE -pthread)

    target_link_libraries(planetary
        -L${SDL2_DIR}/lib
        -L${TAGLIB_DIR}/lib
        glew_static
        -lmingw32
        -lSDL2main
        -lSDL2
        -ltag
        opengl32
        -lshlwapi
        -lversion
        -limm32
        -lsetupapi
        -lwinmm
        -lgdi32
        -lole32
        -loleaut32
        -luuid
        -lcomdlg32
        -lpthread
        -static-libgcc
        -static-libstdc++
        -static
    )

    # Copy SDL2.dll and resources to output
    add_custom_command(TARGET planetary POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL2_DIR}/bin/SDL2.dll"
            "$<TARGET_FILE_DIR:planetary>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources"
            "$<TARGET_FILE_DIR:planetary>/resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/shaders"
            "$<TARGET_FILE_DIR:planetary>/shaders"
    )

else()
    # ===============================================
    # LINUX BUILD (native)
    # ===============================================
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(GLEW REQUIRED glew)
    pkg_check_modules(TAGLIB REQUIRED taglib)
    find_package(OpenGL REQUIRED)

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

endif()

# Copy resources to build directory (for both platforms)
file(COPY resources DESTINATION ${CMAKE_BINARY_DIR})
file(COPY shaders DESTINATION ${CMAKE_BINARY_DIR})
