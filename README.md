# Planetary

Native C++ music visualizer — artists as stars, albums as planets, tracks as moons.

Ported from the original [Bloom Studio Planetary](https://github.com/cooperhewitt/Planetary) iOS app, rebuilt with modern OpenGL and cross-platform libraries.

## Tech Stack

- **SDL2** — window, input, audio
- **OpenGL 3.3+** — GPU-accelerated rendering
- **GLM** — math
- **TagLib** — music metadata parsing
- **stb_image** — texture loading

## Building (Linux)

```bash
# Install dependencies
sudo apt install libsdl2-dev libtag1-dev libglm-dev libglew-dev cmake build-essential

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Running

```bash
# With a music folder
./build/planetary /path/to/music

# Or drag-and-drop a folder onto the window
./build/planetary
```

## Controls

- **Left click** a star — fly to artist, see album planets
- **Right drag** — orbit camera
- **Scroll** — zoom in/out
- **ESC** — zoom back out / quit

## Architecture

Artists are positioned using a golden-angle spiral with hash-based distance (from the original `NodeArtist.cpp`). Colors are derived from ASCII character values in the artist name. Album orbits follow Kepler-like spacing. Track moons orbit albums at speeds proportional to track duration.

## Credits

Original Planetary concept and code by [Bloom Studio](https://github.com/cooperhewitt/Planetary) (Robert Hodgin / flight404), Smithsonian Institution. BSD-3-Clause licensed.
