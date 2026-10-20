#pragma once

#include "media.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

class ArtworkService {
public:
    ArtworkService(std::string source, std::string country);
    ~ArtworkService();

    ArtworkService(const ArtworkService&) = delete;
    ArtworkService& operator=(const ArtworkService&) = delete;

    void start();
    void stop();

    struct Result {
        uint64_t id = 0;
        std::string key;
        std::string url;
        std::string album;
        std::string artist;
        int64_t durationMs = 0;
        bool hasMetadata = false;
    };

    void request(uint64_t id, const TrackInfo& track);
    std::optional<Result> takeResult();

    // Synchronous lookup used by the --lookup diagnostic command.
    Result lookup(const TrackInfo& track);

private:
    struct ItunesInfo {
        std::string artworkUrl;
        std::string album;
        std::string artist;
        int64_t durationMs = 0;
        bool found = false;
    };

    void loop();
    Result resolve(const TrackInfo& track, int64_t requestedAtMs);
    ItunesInfo resolveItunes(const TrackInfo& track);
    std::string resolveNotificationArtwork(int64_t minimumMtimeMs);
    void loadUrlCache();
    void saveUrlCache();

    std::string source_;
    std::string country_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_{false};
    bool hasRequest_ = false;
    uint64_t requestId_ = 0;
    int64_t requestedAtMs_ = 0;
    TrackInfo requestTrack_;
    std::optional<Result> result_;

    std::unordered_map<std::string, Result> metadataCache_;
    std::unordered_map<std::string, std::string> urlCache_;
};
