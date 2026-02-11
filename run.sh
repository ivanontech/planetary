#!/bin/bash
# Launch Planetary with your music library
# Usage: ./run.sh [music_folder_path]
# Default: /home/guy/Music

cd "$(dirname "$0")/build"

MUSIC_PATH="${1:-/home/guy/Music}"

echo "Launching Planetary with music from: $MUSIC_PATH"
./planetary "$MUSIC_PATH"
