#pragma once
// ============================================================
// NAVIDROME / SUBSONIC API CLIENT
// Streams music library metadata from Navidrome over LAN.
// Used on Android TV (Shield) — no local file system needed.
// ============================================================

#include "music_data.h"
#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>

// libcurl for HTTP
#include <curl/curl.h>

// Simple JSON/XML parser (we use Subsonic XML response format)
// Navidrome returns Subsonic XML by default; we parse it manually.

namespace navidrome {

static const std::string ND_USER     = "boss";
static const std::string ND_PASSWORD = "planetary123";
static const std::string ND_CLIENT   = "planetary-android";
static const std::string ND_VERSION  = "1.16.1";
static const std::string ND_FORMAT   = "xml";

// libcurl write callback
static size_t curlWriteCallback(void* data, size_t size, size_t nmemb, std::string* out) {
    size_t total = size * nmemb;
    out->append((char*)data, total);
    return total;
}

// Perform a GET request and return body as string
inline std::string httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string result;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // Don't verify SSL (self-hosted, HTTP only anyway)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[Navidrome] HTTP error: " << curl_easy_strerror(res) << " url=" << url << std::endl;
        return "";
    }
    return result;
}

// Build Subsonic API URL
inline std::string buildUrl(const std::string& serverUrl, const std::string& endpoint,
                             const std::string& extraParams = "") {
    return serverUrl + "/rest/" + endpoint
        + "?u=" + ND_USER
        + "&p=" + ND_PASSWORD
        + "&c=" + ND_CLIENT
        + "&v=" + ND_VERSION
        + "&f=" + ND_FORMAT
        + (extraParams.empty() ? "" : "&" + extraParams);
}

// ============================================================
// MINIMAL XML PARSER HELPERS
// Subsonic XML is simple enough to parse with basic string ops.
// ============================================================

// Extract value of XML attribute: <tag attr="value">
inline std::string xmlAttr(const std::string& tag, const std::string& attr) {
    std::string key = attr + "=\"";
    size_t pos = tag.find(key);
    if (pos == std::string::npos) return "";
    pos += key.size();
    size_t end = tag.find('"', pos);
    if (end == std::string::npos) return "";
    return tag.substr(pos, end - pos);
}

// URL-decode a string
inline std::string urlDecode(const std::string& s) {
    std::string out;
    CURL* curl = curl_easy_init();
    if (curl) {
        int decodedLen;
        char* decoded = curl_easy_unescape(curl, s.c_str(), (int)s.size(), &decodedLen);
        if (decoded) {
            out.assign(decoded, decodedLen);
            curl_free(decoded);
        }
        curl_easy_cleanup(curl);
    }
    return out.empty() ? s : out;
}

// HTML entity decode (basic: &amp; &quot; &apos; &lt; &gt;)
inline std::string entityDecode(const std::string& s) {
    std::string out = s;
    // Simple replacements
    auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(out, "&amp;", "&");
    replaceAll(out, "&quot;", "\"");
    replaceAll(out, "&apos;", "'");
    replaceAll(out, "&lt;", "<");
    replaceAll(out, "&gt;", ">");
    return out;
}

// Parse all XML element tags matching <tagName .../>  or <tagName ...>
inline std::vector<std::string> xmlFindTags(const std::string& xml, const std::string& tagName) {
    std::vector<std::string> results;
    std::string open = "<" + tagName + " ";
    size_t pos = 0;
    while ((pos = xml.find(open, pos)) != std::string::npos) {
        // Find end of this tag
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        results.push_back(xml.substr(pos, end - pos + 1));
        pos = end + 1;
    }
    return results;
}

// ============================================================
// FETCH LIBRARY: artists → albums → tracks from Navidrome
// ============================================================
inline MusicLibrary fetchMusicLibraryFromNavidrome(
    const std::string& serverUrl,
    std::function<void(int, int)> progressCallback = nullptr)
{
    MusicLibrary lib;
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::cout << "[Navidrome] Connecting to: " << serverUrl << std::endl;

    // Step 1: Get all artists
    std::string artistsUrl = buildUrl(serverUrl, "getArtists.view");
    std::string artistsXml = httpGet(artistsUrl);

    if (artistsXml.empty()) {
        std::cerr << "[Navidrome] Failed to fetch artists. Is the server running?" << std::endl;
        curl_global_cleanup();
        return lib;
    }

    // Parse artists
    auto artistTags = xmlFindTags(artistsXml, "artist");
    std::cout << "[Navidrome] Found " << artistTags.size() << " artists" << std::endl;

    int totalArtists = (int)artistTags.size();
    int processed = 0;

    for (auto& artistTag : artistTags) {
        std::string artistId   = xmlAttr(artistTag, "id");
        std::string artistName = entityDecode(xmlAttr(artistTag, "name"));
        if (artistId.empty() || artistName.empty()) continue;

        ArtistData artist;
        artist.name = artistName;

        // Step 2: Get albums for this artist
        std::string albumsUrl = buildUrl(serverUrl, "getArtist.view", "id=" + artistId);
        std::string albumsXml = httpGet(albumsUrl);

        auto albumTags = xmlFindTags(albumsXml, "album");
        for (auto& albumTag : albumTags) {
            std::string albumId   = xmlAttr(albumTag, "id");
            std::string albumName = entityDecode(xmlAttr(albumTag, "name"));
            std::string yearStr   = xmlAttr(albumTag, "year");
            if (albumId.empty()) continue;

            AlbumData album;
            album.name   = albumName.empty() ? "Unknown Album" : albumName;
            album.artist = artistName;
            album.year   = yearStr.empty() ? 0 : std::stoi(yearStr);

            // Step 3: Get tracks for this album
            std::string tracksUrl = buildUrl(serverUrl, "getAlbum.view", "id=" + albumId);
            std::string tracksXml = httpGet(tracksUrl);

            auto songTags = xmlFindTags(tracksXml, "song");
            for (auto& songTag : songTags) {
                TrackData track;
                track.title       = entityDecode(xmlAttr(songTag, "title"));
                track.artist      = entityDecode(xmlAttr(songTag, "artist"));
                track.album       = albumName;
                track.albumArtist = artistName;
                std::string trackNumStr = xmlAttr(songTag, "track");
                std::string durationStr = xmlAttr(songTag, "duration");
                std::string yearStr2    = xmlAttr(songTag, "year");
                track.trackNumber = trackNumStr.empty() ? 0 : std::stoi(trackNumStr);
                track.duration    = durationStr.empty() ? 180.0f : std::stof(durationStr);
                track.year        = yearStr2.empty() ? album.year : std::stoi(yearStr2);
                track.genre       = entityDecode(xmlAttr(songTag, "genre"));

                // The filePath for streaming: use the Subsonic stream URL
                std::string songId = xmlAttr(songTag, "id");
                if (songId.empty()) continue;
                track.filePath = buildUrl(serverUrl, "stream.view", "id=" + songId + "&format=raw&estimateContentLength=true");

                if (track.title.empty()) track.title = "Track " + trackNumStr;
                album.tracks.push_back(track);
            }

            // Sort tracks by track number
            std::sort(album.tracks.begin(), album.tracks.end(),
                [](const TrackData& a, const TrackData& b) {
                    return a.trackNumber < b.trackNumber;
                });

            if (!album.tracks.empty()) {
                artist.albums.push_back(album);
                lib.totalAlbums++;
            }
        }

        if (!artist.albums.empty()) {
            // Determine primary genre
            std::map<std::string, int> genreCounts;
            for (auto& al : artist.albums)
                for (auto& t : al.tracks)
                    if (!t.genre.empty()) genreCounts[t.genre]++;
            if (!genreCounts.empty()) {
                artist.primaryGenre = std::max_element(genreCounts.begin(), genreCounts.end(),
                    [](auto& a, auto& b){ return a.second < b.second; })->first;
            } else {
                artist.primaryGenre = "Unknown";
            }

            // Count tracks
            for (auto& al : artist.albums)
                artist.totalTracks += (int)al.tracks.size();

            // Sort albums by year
            std::sort(artist.albums.begin(), artist.albums.end(),
                [](const AlbumData& a, const AlbumData& b){ return a.year < b.year; });

            lib.artists.push_back(artist);
            lib.totalTracks += artist.totalTracks;
        }

        processed++;
        if (progressCallback) progressCallback(processed, totalArtists);
    }

    // Sort artists by name
    std::sort(lib.artists.begin(), lib.artists.end(),
        [](const ArtistData& a, const ArtistData& b){ return a.name < b.name; });

    std::cout << "[Navidrome] Library: " << lib.artists.size() << " artists, "
              << lib.totalAlbums << " albums, " << lib.totalTracks << " tracks" << std::endl;

    curl_global_cleanup();
    return lib;
}

} // namespace navidrome

// Expose as top-level function for use in main.cpp
inline MusicLibrary fetchMusicLibraryFromNavidrome(
    const std::string& serverUrl,
    std::function<void(int, int)> progressCallback = nullptr)
{
    return navidrome::fetchMusicLibraryFromNavidrome(serverUrl, progressCallback);
}
