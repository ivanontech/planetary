#pragma once

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <cmath>
#include <iostream>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>

namespace fs = std::filesystem;

// ============================================================
// DATA STRUCTURES - mirroring the Electron version's types
// ============================================================

struct TrackData {
    std::string filePath;
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;
    int trackNumber = 0;
    float duration = 0;  // seconds
    int year = 0;
    std::string genre;
};

struct AlbumData {
    std::string name;
    std::string artist;
    int year = 0;
    std::vector<TrackData> tracks;
    // Cover art (raw image data for GL texture creation)
    std::vector<unsigned char> coverArtData;
    int coverArtW = 0, coverArtH = 0;
};

struct ArtistData {
    std::string name;
    std::string primaryGenre; // Most common genre across tracks
    std::vector<AlbumData> albums;
    int totalTracks = 0;
};

struct MusicLibrary {
    std::vector<ArtistData> artists;
    int totalTracks = 0;
    int totalAlbums = 0;
};

// ============================================================
// SCANNER - Port of the Electron version's music:scan IPC
// ============================================================

static const std::vector<std::string> AUDIO_EXTENSIONS = {
    ".mp3", ".flac", ".m4a", ".aac", ".ogg", ".opus",
    ".wav", ".wma", ".aiff", ".alac", ".ape", ".wv"
};

inline bool isAudioFile(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (auto& e : AUDIO_EXTENSIONS) {
        if (ext == e) return true;
    }
    return false;
}

inline std::vector<std::string> scanDirectory(const std::string& dirPath) {
    std::vector<std::string> files;
    try {
        for (auto& entry : fs::recursive_directory_iterator(dirPath,
                fs::directory_options::skip_permission_denied)) {
            if (entry.is_regular_file() && isAudioFile(entry.path().string())) {
                files.push_back(entry.path().string());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[Planetary] Scan error: " << e.what() << std::endl;
    }
    return files;
}

// Extract cover art from an audio file (MP3 ID3v2 or FLAC)
inline std::vector<unsigned char> extractCoverArt(const std::string& path) {
    std::string ext = fs::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".mp3") {
        TagLib::MPEG::File f(path.c_str());
        if (f.isValid() && f.ID3v2Tag()) {
            auto frames = f.ID3v2Tag()->frameListMap()["APIC"];
            if (!frames.isEmpty()) {
                auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (pic) {
                    auto data = pic->picture();
                    return std::vector<unsigned char>(data.begin(), data.end());
                }
            }
        }
    } else if (ext == ".flac") {
        TagLib::FLAC::File f(path.c_str());
        if (f.isValid()) {
            auto pics = f.pictureList();
            if (!pics.isEmpty()) {
                auto data = pics.front()->data();
                return std::vector<unsigned char>(data.begin(), data.end());
            }
        }
    }
    return {};
}

inline MusicLibrary scanMusicLibrary(const std::string& dirPath,
    std::function<void(int, int)> progressCallback = nullptr)
{
    std::cout << "[Planetary] Scanning: " << dirPath << std::endl;
    auto files = scanDirectory(dirPath);
    std::cout << "[Planetary] Found " << files.size() << " audio files" << std::endl;

    // Parse metadata with TagLib
    std::map<std::string, std::map<std::string, AlbumData>> artistAlbums;
    int scanned = 0;

    for (auto& filePath : files) {
        TrackData track;
        track.filePath = filePath;

        TagLib::FileRef f(filePath.c_str());
        if (!f.isNull() && f.tag()) {
            auto* tag = f.tag();
            track.title = tag->title().toCString(true);
            track.artist = tag->artist().toCString(true);
            track.album = tag->album().toCString(true);
            track.trackNumber = tag->track();
            track.year = tag->year();
            track.genre = tag->genre().toCString(true);

            if (f.audioProperties()) {
                track.duration = f.audioProperties()->lengthInSeconds();
            }
        }

        // Fallbacks (same as Electron version)
        if (track.title.empty()) {
            track.title = fs::path(filePath).stem().string();
        }
        if (track.artist.empty()) {
            track.artist = fs::path(filePath).parent_path().filename().string();
        }
        if (track.album.empty()) {
            track.album = fs::path(filePath).parent_path().filename().string();
        }
        if (track.duration <= 0) track.duration = 180.0f;

        track.albumArtist = track.artist;

        // Group by artist → album
        artistAlbums[track.albumArtist][track.album].name = track.album;
        artistAlbums[track.albumArtist][track.album].artist = track.albumArtist;
        artistAlbums[track.albumArtist][track.album].year = track.year;
        artistAlbums[track.albumArtist][track.album].tracks.push_back(track);

        scanned++;
        if (progressCallback && scanned % 50 == 0) {
            progressCallback(scanned, (int)files.size());
        }
    }

    // Build the library structure
    MusicLibrary lib;
    for (auto& [artistName, albums] : artistAlbums) {
        ArtistData artist;
        artist.name = artistName;
        for (auto& [albumName, albumData] : albums) {
            // Sort tracks by track number
            std::sort(albumData.tracks.begin(), albumData.tracks.end(),
                [](const TrackData& a, const TrackData& b) {
                    return a.trackNumber < b.trackNumber;
                });
            artist.albums.push_back(albumData);
            artist.totalTracks += (int)albumData.tracks.size();
            lib.totalAlbums++;
        }
        // Determine primary genre from most common genre across tracks
        std::map<std::string, int> genreCounts;
        for (auto& album : artist.albums)
            for (auto& t : album.tracks)
                if (!t.genre.empty() && t.genre != "Unknown") genreCounts[t.genre]++;
        if (!genreCounts.empty()) {
            artist.primaryGenre = std::max_element(genreCounts.begin(), genreCounts.end(),
                [](auto& a, auto& b) { return a.second < b.second; })->first;
        } else {
            artist.primaryGenre = "Unknown";
        }

        // Extract cover art for each album (from first track)
        for (auto& album : artist.albums) {
            if (album.coverArtData.empty() && !album.tracks.empty()) {
                album.coverArtData = extractCoverArt(album.tracks[0].filePath);
            }
        }

        // Sort albums by year
        std::sort(artist.albums.begin(), artist.albums.end(),
            [](const AlbumData& a, const AlbumData& b) {
                return a.year < b.year;
            });
        lib.artists.push_back(artist);
        lib.totalTracks += artist.totalTracks;
    }

    // Sort artists by name
    std::sort(lib.artists.begin(), lib.artists.end(),
        [](const ArtistData& a, const ArtistData& b) {
            return a.name < b.name;
        });

    std::cout << "[Planetary] Library: " << lib.artists.size() << " artists, "
              << lib.totalAlbums << " albums, " << lib.totalTracks << " tracks" << std::endl;
    return lib;
}
