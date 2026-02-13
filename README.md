# Planetary

A beautiful 3D music visualizer that transforms your music library into an interactive universe — **artists as stars, albums as planets, tracks as moons**.

![Planetary Screenshot](https://raw.githubusercontent.com/ivanontech/planetary/main/screenshots/planetary.png)

Ported from the original [Bloom Studio Planetary](https://github.com/cooperhewitt/Planetary) iOS app, rebuilt with modern OpenGL and cross-platform support for **Windows, Linux, and macOS**.

## Download

**[Download Latest Release](https://github.com/ivanontech/planetary/releases/latest)**

Pre-built binaries available for:
- Windows (64-bit)
- Linux (64-bit)
- macOS (Intel & Apple Silicon)

## Features

- **Interactive 3D Universe** — Navigate through your music collection as a stunning space visualization
- **Audio-Reactive Effects** — Stars pulse and emit solar flares synced to the music's bass and rhythm
- **Rich Visual Effects**
  - Multi-layer nebula clouds (red and blue gas)
  - Comet particles with glowing tails
  - Saturn-like rings on large albums (10+ tracks)
  - Orbital particles and dark matter effects
  - Dynamic star coronas with artist-colored flames
- **Gamepad Support** — Full controller support including Steam Input
- **Search** — Find any artist instantly with keyboard or on-screen virtual keyboard

## Controls

### Mouse & Keyboard
| Control | Action |
|---------|--------|
| **Left Click** star | Fly to artist, view album planets |
| **Left Click** planet | View track moons |
| **Left Click** moon | Play track |
| **Right Drag** | Orbit camera |
| **Scroll** | Zoom in/out |
| **Type** | Search for artists |
| **ESC** | Zoom out / Exit |
| **Space** | Toggle auto-rotate |

### Gamepad (Xbox/PlayStation/Steam)
| Control | Action |
|---------|--------|
| **A / Cross** | Select / Play |
| **B / Circle** | Back / Zoom out |
| **Left Stick** | Pan camera |
| **Right Stick** | Orbit camera |
| **D-Pad** | Navigate albums/search |
| **Triggers** | Zoom in/out |
| **Bumpers** | Previous/Next track |
| **L3 (Left Stick Click)** | Recenter to now playing |
| **R3 (Right Stick Click)** | Toggle virtual keyboard |
| **Guide/Home** | Toggle auto-rotate |

## Building from Source

### Dependencies

- CMake 3.16+
- C++17 compiler
- SDL2
- GLEW
- GLM
- TagLib

### Linux

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install build-essential cmake libsdl2-dev libglew-dev libglm-dev libtag1-dev

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run
./planetary /path/to/music
```

### macOS

```bash
# Install dependencies
brew install cmake sdl2 glew glm taglib

# Build
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)

# Run
./planetary /path/to/music
```

### Windows (MSYS2/MinGW)

```bash
# Install dependencies
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 \
          mingw-w64-x86_64-glew mingw-w64-x86_64-glm mingw-w64-x86_64-taglib

# Build
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
mingw32-make -j$(nproc)
```

### Cross-compile for Windows (from Linux)

```bash
sudo apt install mingw-w64
mkdir build-win && cd build-win
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/mingw-toolchain.cmake ..
make -j$(nproc)
```

## Architecture

- **Artists** — Positioned using a golden-angle spiral with hash-based distance. Colors derived from artist name ASCII values.
- **Albums** — Orbit stars with Kepler-like spacing. Album art displayed as planet textures.
- **Tracks** — Moons orbiting albums at speeds proportional to track duration.
- **Audio Analysis** — Real-time RMS, bass, and treble analysis drives visual effects.

## Tech Stack

- **SDL2** — Window, input, audio playback
- **OpenGL 3.3** — GPU-accelerated rendering
- **GLM** — Vector/matrix math
- **TagLib** — Music metadata (ID3, FLAC, etc.)
- **Dear ImGui** — User interface
- **miniaudio** — Audio decoding and playback
- **stb_image** — Texture loading

## Credits

Original Planetary concept and code by [Bloom Studio](https://github.com/cooperhewitt/Planetary) (Robert Hodgin / flight404), Smithsonian Institution.

## License

BSD-3-Clause — See [LICENSE](LICENSE) for details.
