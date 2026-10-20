#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct AmazonCachedTrack {
    std::string asin;
    std::string title;
    std::string artist;
    std::string album;
    int64_t durationMs = 0;
};

// Reads the current track metadata from Amazon Music's local catalog cache.
// Amazon sometimes exposes an empty media session even though its own player
// bar and cache contain the track. This fallback works while the app is
// minimized because it does not depend on screen capture.
std::optional<AmazonCachedTrack> findAmazonCachedTrack();
