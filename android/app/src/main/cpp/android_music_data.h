#pragma once

// Android-specific music_data.h — uses Navidrome HTTP API instead of TagLib
// Connects to Navidrome at http://10.0.0.2:4533 (Mac Studio LAN)

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <functional>
#include <cmath>
#include <android/log.h>
#include <sstream>

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "Planetary", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Planetary", __VA_ARGS__)

// ============================================================
// DATA STRUCTURES (same interface as desktop music_data.h)
// ============================================================

struct TrackData {
    std::string filePath;   // On Android: Navidrome stream URL
    std::string id;         // Navidrome track ID
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;
    int trackNumber = 0;
    float duration = 0;
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
    std::string primaryGenre;
    std::vector<AlbumData> albums;
    int totalTracks = 0;
};

struct MusicLibrary {
    std::vector<ArtistData> artists;
    int totalTracks = 0;
    int totalAlbums = 0;
};

// ============================================================
// NAVIDROME CONFIG
// ============================================================
static const char* NAVIDROME_BASE = "http://10.0.0.2:4533";
static const char* NAVIDROME_USER = "admin";
static const char* NAVIDROME_PASS = "planetary";  // Set real password here

// Build Subsonic API URL
inline std::string subsonicUrl(const std::string& endpoint, const std::string& params = "") {
    std::string url = std::string(NAVIDROME_BASE) + "/rest/" + endpoint + ".view"
        + "?u=" + NAVIDROME_USER
        + "&p=" + NAVIDROME_PASS
        + "&v=1.16.1&c=planetary&f=json";
    if (!params.empty()) url += "&" + params;
    return url;
}

// Build streaming URL for a track ID
inline std::string streamUrl(const std::string& id) {
    return subsonicUrl("stream", "id=" + id + "&maxBitRate=320&format=mp3");
}

// Build cover art URL for an album
inline std::string coverArtUrl(const std::string& id) {
    return subsonicUrl("getCoverArt", "id=" + id + "&size=300");
}

// ============================================================
// MINIMAL JSON PARSER (no external dep)
// Very limited — only handles what Navidrome returns for artists/albums/songs
// ============================================================
static std::string jsonExtractString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) {
        // Try numeric
        search = "\"" + key + "\":";
        pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = json.find_first_of(",}", pos);
        std::string val = json.substr(pos, end - pos);
        // Remove quotes if present
        if (!val.empty() && val[0] == '"') val = val.substr(1, val.size()-2);
        return val;
    }
    pos += search.size();
    auto end = pos;
    // Handle escaped quotes
    while (end < json.size()) {
        if (json[end] == '\\') { end += 2; continue; }
        if (json[end] == '"') break;
        end++;
    }
    return json.substr(pos, end - pos);
}

static int jsonExtractInt(const std::string& json, const std::string& key, int def = 0) {
    std::string s = jsonExtractString(json, key);
    if (s.empty()) return def;
    try { return std::stoi(s); } catch(...) { return def; }
}

static float jsonExtractFloat(const std::string& json, const std::string& key, float def = 0.0f) {
    std::string s = jsonExtractString(json, key);
    if (s.empty()) return def;
    try { return std::stof(s); } catch(...) { return def; }
}

// Extract array of JSON objects (one level deep)
static std::vector<std::string> jsonExtractArray(const std::string& json, const std::string& arrayKey) {
    std::vector<std::string> result;
    std::string search = "\"" + arrayKey + "\":[";
    auto pos = json.find(search);
    if (pos == std::string::npos) return result;
    pos += search.size();
    
    int depth = 1;
    auto start = pos;
    bool inObj = false;
    size_t objStart = 0;
    
    while (pos < json.size() && depth > 0) {
        char c = json[pos];
        if (c == '{') {
            if (!inObj) { inObj = true; objStart = pos; }
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 1 && inObj) {
                result.push_back(json.substr(objStart, pos - objStart + 1));
                inObj = false;
            }
        } else if (c == ']' && depth == 1) {
            break;
        }
        pos++;
    }
    return result;
}

// ============================================================
// HTTP GET — using libcurl (linked via NDK)
// On Android we can use Java HTTP or libcurl. We'll use SDL_net
// or miniaudio's built-in HTTP. For simplicity, use blocking POSIX sockets.
// ============================================================
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

// Very simple HTTP GET — returns response body, empty on error
// Only handles http://, no HTTPS
static std::string httpGet(const std::string& url, int timeoutSec = 10) {
    // Parse URL: http://host:port/path?query
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
    
    // Resolve hostname
    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0) {
        LOGE("[HTTP] Failed to resolve host: %s", host.c_str());
        return "";
    }
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { freeaddrinfo(res); return ""; }
    
    // Set timeout
    struct timeval tv;
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        LOGE("[HTTP] Failed to connect to %s:%d", host.c_str(), port);
        close(sock);
        freeaddrinfo(res);
        return "";
    }
    freeaddrinfo(res);
    
    // Send HTTP request
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    send(sock, req.c_str(), req.size(), 0);
    
    // Read response
    std::string response;
    char buf[4096];
    ssize_t n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, n);
    }
    close(sock);
    
    // Strip HTTP headers
    auto headerEnd = response.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        return response.substr(headerEnd + 4);
    }
    return response;
}

// ============================================================
// SCAN MUSIC LIBRARY via Navidrome Subsonic API
// ============================================================
MusicLibrary scanMusicLibrary(
    const std::string& /*musicPath*/,   // unused on Android
    std::function<void(int,int,const std::string&)> /*progress*/ = nullptr
) {
    MusicLibrary lib;
    
    LOGD("[Planetary] Fetching library from Navidrome at %s", NAVIDROME_BASE);
    
    // Get all artists
    std::string artistsUrl = subsonicUrl("getArtists");
    std::string artistsJson = httpGet(artistsUrl);
    
    if (artistsJson.empty()) {
        LOGE("[Planetary] Failed to fetch artists from Navidrome");
        // Return demo data so the visualizer still works
        ArtistData demo;
        demo.name = "Navidrome (Offline)";
        demo.primaryGenre = "Electronic";
        AlbumData demoAlbum;
        demoAlbum.name = "Demo Album";
        demoAlbum.artist = demo.name;
        TrackData demoTrack;
        demoTrack.title = "Demo Track";
        demoTrack.artist = demo.name;
        demoTrack.album = demoAlbum.name;
        demoTrack.duration = 240.0f;
        demoAlbum.tracks.push_back(demoTrack);
        demo.albums.push_back(demoAlbum);
        demo.totalTracks = 1;
        lib.artists.push_back(demo);
        lib.totalTracks = 1;
        lib.totalAlbums = 1;
        return lib;
    }
    
    // Parse artists from JSON
    // Navidrome returns: subsonic-response -> artists -> index -> [artist]
    // We'll extract artist entries from the JSON
    auto extractArtistEntries = [&](const std::string& json) -> std::vector<std::string> {
        std::vector<std::string> result;
        // Find all "artist":[...] arrays inside index objects
        size_t pos = 0;
        while (true) {
            auto found = json.find("\"artist\":[", pos);
            if (found == std::string::npos) break;
            auto arr = jsonExtractArray(json.substr(found), "artist");
            for (auto& a : arr) result.push_back(a);
            pos = found + 10;
        }
        return result;
    };
    
    auto artistEntries = extractArtistEntries(artistsJson);
    LOGD("[Planetary] Found %zu artists", artistEntries.size());
    
    int artistCount = 0;
    for (auto& aJson : artistEntries) {
        std::string artistId = jsonExtractString(aJson, "id");
        std::string artistName = jsonExtractString(aJson, "name");
        if (artistId.empty() || artistName.empty()) continue;
        
        ArtistData artist;
        artist.name = artistName;
        
        // Get albums for this artist
        std::string albumsUrl = subsonicUrl("getArtist", "id=" + artistId);
        std::string albumsJson = httpGet(albumsUrl);
        if (albumsJson.empty()) continue;
        
        auto albumEntries = jsonExtractArray(albumsJson, "album");
        for (auto& albJson : albumEntries) {
            std::string albumId = jsonExtractString(albJson, "id");
            std::string albumName = jsonExtractString(albJson, "name");
            if (albumId.empty()) continue;
            
            AlbumData album;
            album.name = albumName.empty() ? "Unknown Album" : albumName;
            album.artist = artistName;
            album.id = albumId;
            album.year = jsonExtractInt(albJson, "year");
            
            // Get tracks for this album
            std::string tracksUrl = subsonicUrl("getAlbum", "id=" + albumId);
            std::string tracksJson = httpGet(tracksUrl);
            
            auto trackEntries = jsonExtractArray(tracksJson, "song");
            for (auto& tJson : trackEntries) {
                std::string trackId = jsonExtractString(tJson, "id");
                if (trackId.empty()) continue;
                
                TrackData track;
                track.id = trackId;
                track.filePath = streamUrl(trackId);  // Navidrome stream URL
                track.title = jsonExtractString(tJson, "title");
                if (track.title.empty()) track.title = "Unknown Track";
                track.artist = jsonExtractString(tJson, "artist");
                if (track.artist.empty()) track.artist = artistName;
                track.album = album.name;
                track.albumArtist = artistName;
                track.trackNumber = jsonExtractInt(tJson, "track");
                track.duration = jsonExtractFloat(tJson, "duration");
                track.year = jsonExtractInt(tJson, "year");
                track.genre = jsonExtractString(tJson, "genre");
                
                album.tracks.push_back(track);
                artist.totalTracks++;
                lib.totalTracks++;
            }
            
            if (!album.tracks.empty()) {
                artist.albums.push_back(album);
                lib.totalAlbums++;
            }
        }
        
        // Sort albums by year
        std::sort(artist.albums.begin(), artist.albums.end(),
            [](const AlbumData& a, const AlbumData& b) { return a.year < b.year; });
        
        if (artist.totalTracks > 0) {
            lib.artists.push_back(artist);
        }
        
        artistCount++;
        if (artistCount > 50) break;  // Limit initial load
    }
    
    // Sort artists by name
    std::sort(lib.artists.begin(), lib.artists.end(),
        [](const ArtistData& a, const ArtistData& b) { return a.name < b.name; });
    
    LOGD("[Planetary] Library: %zu artists, %d albums, %d tracks",
         lib.artists.size(), lib.totalAlbums, lib.totalTracks);
    return lib;
}

// Stub for cover art extraction (Android uses URL-based cover art instead)
inline std::vector<unsigned char> extractCoverArt(const std::string& /*filePath*/) {
    return {};  // No TagLib on Android
}
