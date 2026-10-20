#pragma once

#include "config.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct WorkerSnapshot {
    bool running = false;
    bool rpcEnabled = false;
    bool discordConnected = false;
    bool hasTrack = false;
    bool playing = false;
    bool paused = false;
    std::string title;
    std::string artist;
    std::string album;
    std::string sourceApp;
    std::string artworkUrl;
    int64_t positionMs = 0;
    int64_t durationMs = 0;
    std::string discordError;
    std::string lastError;
    std::string message;
};

// Runs the media -> metadata -> Discord loop on a background thread while the
// Win32 message loop stays responsive.
class RpcWorker {
public:
    RpcWorker();
    ~RpcWorker();

    RpcWorker(const RpcWorker&) = delete;
    RpcWorker& operator=(const RpcWorker&) = delete;

    void start(const Config& config);
    void stop();
    bool isRunning() const;
    WorkerSnapshot snapshot() const;

private:
    void run(Config config);
    void publish(const WorkerSnapshot& snapshot);

    std::thread thread_;
    mutable std::mutex mutex_;
    WorkerSnapshot snapshot_;
    std::atomic<bool> stop_{false};
};
