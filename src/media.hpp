#pragma once

#include <winrt/Windows.Media.Control.h>

#include <cstdint>
#include <string>
#include <vector>

struct TrackInfo {
    bool hasSession = false;
    bool playing = false;
    bool paused = false;
    std::string sourceApp;
    std::string title;
    std::string artist;
    std::string album;
    std::string albumArtist;
    std::string subtitle;
    int64_t positionMs = 0;
    int64_t durationMs = 0;
    std::vector<uint8_t> thumbnail;
    std::string thumbnailType; // "png", "jpg", ...

    bool hasMetadata() const {
        return hasSession && !title.empty();
    }

    std::string key() const {
        // Album is included so that two tracks with the same title/artist still differ.
        return title + "\x1f" + artist + "\x1f" + album;
    }
};

class MediaWatcher {
public:
    MediaWatcher();
    ~MediaWatcher();

    bool initialize(const std::string& appFilter, bool needThumbnail, std::string& error);
    TrackInfo poll();

    struct SessionSummary {
        std::string appId;
        std::string title;
        std::string artist;
        std::string status;
    };
    std::vector<SessionSummary> listSessions();

private:
    bool matchesFilter(const std::string& appId) const;
    static std::string statusName(
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus status);

    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager manager_{nullptr};
    std::vector<std::string> filters_;
    bool needThumbnail_ = false;
    std::string lastKey_;
    TrackInfo lastInfo_;
    std::string lastCacheKey_;
    int64_t lastCacheAttemptMs_ = 0;
};
