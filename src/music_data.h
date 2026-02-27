#pragma once

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <functional>
#include <cmath>
#include <iostream>

#ifndef __ANDROID__
#include <filesystem>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
namespace fs = std::filesystem;
#endif

// ============================================================
// DATA STRUCTURES - mirroring the Electron version's types
// ============================================================

struct TrackData {
    std::string filePath;
    std::string id;         // Navidrome track ID (Android streaming)
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
    std::string id;         // Navidrome album ID (for cover art URL)
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
// (Desktop only — Android uses Navidrome HTTP API)
// ============================================================
#ifndef __ANDROID__

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

#endif // !__ANDROID__ (desktop scanner)

// ============================================================
// ANDROID: Fetch music library from Navidrome via Subsonic API
// ============================================================
#ifdef __ANDROID__

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <android/log.h>

#define PLANETARY_LOG(...) __android_log_print(ANDROID_LOG_DEBUG, "Planetary", __VA_ARGS__)

// Simple HTTP GET (blocking, no SSL)
static std::string planetaryHttpGet(const std::string& url, int timeoutSec = 10) {
    std::string host, path;
    int port = 80;

    std::string u = url;
    if (u.substr(0, 7) == "http://") u = u.substr(7);
    auto slashPos = u.find('/');
    std::string hostPort = (slashPos != std::string::npos) ? u.substr(0, slashPos) : u;
    path = (slashPos != std::string::npos) ? u.substr(slashPos) : "/";
    auto colonPos = hostPort.find(':');
    if (colonPos != std::string::npos) {
        host = hostPort.substr(0, colonPos);
        port = std::stoi(hostPort.substr(colonPos + 1));
    } else {
        host = hostPort;
    }

    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        PLANETARY_LOG("[HTTP] Failed to resolve host: %s", host.c_str());
        return "";
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { freeaddrinfo(res); return ""; }

    struct timeval tv;
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        PLANETARY_LOG("[HTTP] Failed to connect to %s:%d", host.c_str(), port);
        close(sock);
        freeaddrinfo(res);
        return "";
    }
    freeaddrinfo(res);

    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    send(sock, req.c_str(), req.size(), 0);

    std::string response;
    char buf[4096];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, n);
    }
    close(sock);

    auto headerEnd = response.find("\r\n\r\n");
    if (headerEnd != std::string::npos) return response.substr(headerEnd + 4);
    return response;
}

// Minimal JSON string extractor
static std::string naviJsonStr(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) {
        // Try numeric value
        search = "\"" + key + "\":";
        pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = json.find_first_of(",}", pos);
        std::string val = json.substr(pos, end - pos);
        if (!val.empty() && val[0] == '"') val = val.substr(1, val.size()-2);
        return val;
    }
    pos += search.size();
    auto end = pos;
    while (end < json.size()) {
        if (json[end] == '\\') { end += 2; continue; }
        if (json[end] == '"') break;
        end++;
    }
    return json.substr(pos, end - pos);
}

static int naviJsonInt(const std::string& json, const std::string& key, int def = 0) {
    std::string s = naviJsonStr(json, key);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch(...) { return def; }
}

static float naviJsonFloat(const std::string& json, const std::string& key, float def = 0.0f) {
    std::string s = naviJsonStr(json, key);
    if (s.empty()) return def;
    try { return std::stof(s); } catch(...) { return def; }
}

static std::vector<std::string> naviJsonArray(const std::string& json, const std::string& arrayKey) {
    std::vector<std::string> result;
    std::string search = "\"" + arrayKey + "\":[";
    auto pos = json.find(search);
    if (pos == std::string::npos) return result;
    pos += search.size();

    int depth = 0;
    bool inObj = false;
    size_t objStart = 0;

    while (pos < json.size()) {
        char c = json[pos];
        if (c == '{') {
            if (!inObj) { inObj = true; objStart = pos; }
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && inObj) {
                result.push_back(json.substr(objStart, pos - objStart + 1));
                inObj = false;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
        pos++;
    }
    return result;
}

// Navidrome connection settings (Mac Studio LAN IP)
static const char* NAVI_BASE = "http://10.0.0.73:4533";
static const char* NAVI_USER = "boss";
static const char* NAVI_PASS = "planetary123";

static std::string naviUrl(const std::string& endpoint, const std::string& params = "") {
    std::string url = std::string(NAVI_BASE) + "/rest/" + endpoint + ".view"
        + "?u=" + NAVI_USER
        + "&p=" + NAVI_PASS
        + "&v=1.16.1&c=planetary&f=json";
    if (!params.empty()) url += "&" + params;
    return url;
}

inline MusicLibrary fetchMusicLibraryFromNavidrome(
    const std::string& /*serverUrl*/,
    std::function<void(int,int)> progressCallback = nullptr
) {
    MusicLibrary lib;
    PLANETARY_LOG("[Planetary] Connecting to Navidrome at %s", NAVI_BASE);

    std::string artistsJson = planetaryHttpGet(naviUrl("getArtists"));
    if (artistsJson.empty()) {
        PLANETARY_LOG("[Planetary] Failed to reach Navidrome, using demo library");
        ArtistData demo;
        demo.name = "Navidrome Offline";
        demo.primaryGenre = "Electronic";
        AlbumData alb;
        alb.name = "Demo Album";
        alb.artist = demo.name;
        TrackData trk;
        trk.title = "Connect to Navidrome";
        trk.artist = demo.name;
        trk.album = alb.name;
        trk.duration = 240.0f;
        alb.tracks.push_back(trk);
        demo.albums.push_back(alb);
        demo.totalTracks = 1;
        lib.artists.push_back(demo);
        lib.totalTracks = 1; lib.totalAlbums = 1;
        return lib;
    }

    // Extract all artist entries from index groups
    std::vector<std::string> artistEntries;
    size_t pos = 0;
    while (true) {
        auto found = artistsJson.find("\"artist\":[", pos);
        if (found == std::string::npos) break;
        auto arr = naviJsonArray(artistsJson.substr(found), "artist");
        for (auto& a : arr) artistEntries.push_back(a);
        pos = found + 10;
    }

    PLANETARY_LOG("[Planetary] Found %zu artists", artistEntries.size());
    int totalArtists = (int)artistEntries.size();
    int processed = 0;

    for (auto& aJson : artistEntries) {
        std::string artistId = naviJsonStr(aJson, "id");
        std::string artistName = naviJsonStr(aJson, "name");
        if (artistId.empty() || artistName.empty()) { processed++; continue; }

        ArtistData artist;
        artist.name = artistName;

        std::string albumsJson = planetaryHttpGet(naviUrl("getArtist", "id=" + artistId));
        auto albumEntries = naviJsonArray(albumsJson, "album");

        for (auto& albJson : albumEntries) {
            std::string albumId = naviJsonStr(albJson, "id");
            if (albumId.empty()) continue;

            AlbumData album;
            album.name = naviJsonStr(albJson, "name");
            if (album.name.empty()) album.name = "Unknown Album";
            album.id = albumId;
            album.artist = artistName;
            album.year = naviJsonInt(albJson, "year");

            std::string tracksJson = planetaryHttpGet(naviUrl("getAlbum", "id=" + albumId));
            auto trackEntries = naviJsonArray(tracksJson, "song");

            for (auto& tJson : trackEntries) {
                std::string trackId = naviJsonStr(tJson, "id");
                if (trackId.empty()) continue;

                TrackData track;
                track.id = trackId;
                track.filePath = naviUrl("stream", "id=" + trackId + "&maxBitRate=320&format=mp3");
                track.title = naviJsonStr(tJson, "title");
                if (track.title.empty()) track.title = "Unknown Track";
                track.artist = naviJsonStr(tJson, "artist");
                if (track.artist.empty()) track.artist = artistName;
                track.album = album.name;
                track.albumArtist = artistName;
                track.trackNumber = naviJsonInt(tJson, "track");
                track.duration = naviJsonFloat(tJson, "duration");
                track.year = naviJsonInt(tJson, "year");
                track.genre = naviJsonStr(tJson, "genre");

                album.tracks.push_back(track);
                artist.totalTracks++;
                lib.totalTracks++;
            }

            if (!album.tracks.empty()) {
                artist.albums.push_back(album);
                lib.totalAlbums++;
            }
        }

        std::sort(artist.albums.begin(), artist.albums.end(),
            [](const AlbumData& a, const AlbumData& b) { return a.year < b.year; });

        if (artist.totalTracks > 0) lib.artists.push_back(artist);

        processed++;
        if (progressCallback) progressCallback(processed, totalArtists);
    }

    std::sort(lib.artists.begin(), lib.artists.end(),
        [](const ArtistData& a, const ArtistData& b) { return a.name < b.name; });

    PLANETARY_LOG("[Planetary] Library loaded: %zu artists, %d albums, %d tracks",
        lib.artists.size(), lib.totalAlbums, lib.totalTracks);
    return lib;
}

#endif // __ANDROID__
