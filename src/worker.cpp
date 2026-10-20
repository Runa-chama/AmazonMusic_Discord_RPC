#include "worker.hpp"

#include "amazon_log.hpp"
#include "artwork.hpp"
#include "discord_ipc.hpp"
#include "json.hpp"
#include "media.hpp"
#include "util.hpp"

#include <windows.h>
#include <winrt/base.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <string>

namespace {

int statusDisplayType(const std::string& value) {
    // 0 = application name, 1 = state, 2 = details.
    if (value == "name") return 0;
    if (value == "state") return 1;
    return 2;
}

std::string buildActivity(const TrackInfo& track, const std::string& artworkUrl,
                          const std::string& statusDisplay) {
    const std::string title = json::truncateUtf8(track.title, 128);
    std::string artist = track.artist;
    if (artist.empty() || artist == track.title) artist = "Amazon Music";
    artist = json::truncateUtf8(artist, 128);

    std::string activity = "{\"type\":2";
    activity += ",\"details\":" + json::quote(title);
    activity += ",\"state\":" + json::quote(artist);
    activity += ",\"status_display_type\":" + std::to_string(statusDisplayType(statusDisplay));

    if (track.playing && track.durationMs > 0) {
        const int64_t nowSeconds = unixNowMs() / 1000;
        int64_t positionSeconds = track.positionMs / 1000;
        const int64_t durationSeconds = track.durationMs / 1000;
        if (positionSeconds < 0) positionSeconds = 0;
        if (positionSeconds > durationSeconds) positionSeconds = durationSeconds;
        const int64_t start = nowSeconds - positionSeconds;
        const int64_t end = start + durationSeconds;
        if (end > nowSeconds) {
            activity += ",\"timestamps\":{\"start\":" + std::to_string(start) +
                        ",\"end\":" + std::to_string(end) + "}";
        }
    }

    if (!artworkUrl.empty()) {
        const std::string largeText =
            json::truncateUtf8(track.album.empty() ? track.title : track.album, 128);
        activity += ",\"assets\":{\"large_image\":" + json::quote(artworkUrl) +
                    ",\"large_text\":" + json::quote(largeText) + "}";
    }

    activity += ",\"instance\":false}";
    return activity;
}

std::string formatTrackLine(const TrackInfo& track) {
    std::string line = track.title;
    if (!track.artist.empty()) line += " — " + track.artist;
    if (track.durationMs > 0) {
        line += " (" + formatDuration(track.positionMs) + "/" +
                formatDuration(track.durationMs) + ")";
    }
    return line;
}

} // namespace

RpcWorker::RpcWorker() = default;

RpcWorker::~RpcWorker() {
    stop();
}

void RpcWorker::publish(const WorkerSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_ = snapshot;
}

WorkerSnapshot RpcWorker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool RpcWorker::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_.running;
}

void RpcWorker::start(const Config& config) {
    stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = WorkerSnapshot{};
        snapshot_.rpcEnabled = config.rpcEnabled;
        snapshot_.message = "停止中";
        if (!config.rpcEnabled) {
            snapshot_.message = "RPC は無効です";
            return;
        }
        if (!isValidClientId(config.clientId)) {
            snapshot_.lastError = "Discord Application ID を設定してください。";
            snapshot_.message = "設定が必要です";
            return;
        }
        snapshot_.running = true;
        snapshot_.message = "起動中";
    }

    stop_.store(false);
    try {
        thread_ = std::thread(&RpcWorker::run, this, config);
    } catch (const std::exception& ex) {
        const std::string error = std::string("ワーカースレッドを開始できませんでした: ") + ex.what();
        logError(error);
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.running = false;
        snapshot_.lastError = error;
        snapshot_.message = "エラー";
    }
}

void RpcWorker::stop() {
    stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    stop_.store(false);

    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.running = false;
    snapshot_.discordConnected = false;
    snapshot_.hasTrack = false;
    snapshot_.playing = false;
    snapshot_.paused = false;
    snapshot_.message = snapshot_.rpcEnabled ? "停止中" : "RPC は無効です";
}

void RpcWorker::run(Config config) {
    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
    } catch (const winrt::hresult_error& ex) {
        const std::string error = "WinRT を初期化できませんでした: " +
                                  utf8FromWide(ex.message().c_str());
        logError(error);
        WorkerSnapshot snapshot;
        snapshot.rpcEnabled = true;
        snapshot.lastError = error;
        snapshot.message = "エラー";
        publish(snapshot);
        return;
    }

    {
        MediaWatcher media;
        std::string mediaError;
        if (!media.initialize(config.appFilter, false, mediaError)) {
            logError(mediaError);
            WorkerSnapshot snapshot;
            snapshot.rpcEnabled = true;
            snapshot.lastError = mediaError;
            snapshot.message = "メディア情報を取得できません";
            publish(snapshot);
        } else {
            ArtworkService artwork(config.artSource, config.artCountry);
            artwork.start();

            DiscordIpc discord(config.clientId);
            std::string reportedDiscordError;
            if (!discord.connect()) {
                const std::string connectionError = discord.lastError();
                if (!connectionError.empty()) {
                    logError("Discord に接続できません: " + connectionError);
                    reportedDiscordError = connectionError;
                } else {
                    logInfo("Discord が見つかりません。Discord デスクトップアプリを起動すると自動で接続します。");
                }
            }

            auto lastConnectAttempt = std::chrono::steady_clock::now();
            bool forceSend = true;
            bool activityShown = false;

            std::string currentKey;
            std::string artworkUrl;
            ArtworkService::Result metadata;
            bool hasMetadata = false;
            uint64_t artworkRequestId = 0;

            int64_t positionBaseMs = 0;
            int64_t positionBaseAtMs = unixNowMs();
            bool positionWasPlaying = false;

            std::string lastSentKey;
            std::string lastSentArtwork;
            bool lastSentPlaying = false;
            int64_t lastSentAtMs = 0;
            int64_t lastSentPositionMs = 0;

            WorkerSnapshot status;
            status.running = true;
            status.rpcEnabled = true;
            status.discordConnected = discord.isConnected();
            status.discordError = reportedDiscordError;
            status.message = "待機中";
            publish(status);

            logInfo("Amazon Music の再生を待機しています...");

            while (!stop_.load()) {
                const int64_t now = unixNowMs();
                discord.poll();
                const TrackInfo track = media.poll();
                const bool newTrack = track.hasMetadata() && track.key() != currentKey;

                if (newTrack) {
                    currentKey = track.key();
                    hasMetadata = false;
                    metadata = ArtworkService::Result{};
                    artworkUrl.clear();
                    artwork.request(++artworkRequestId, track);
                    forceSend = true;

                    positionBaseMs = 0;
                    positionBaseAtMs = now;
                    if (track.positionMs > 0) {
                        positionBaseMs = track.positionMs;
                    } else if (track.playing) {
                        if (const auto anchor = lastAmazonPlaybackTerminationUnixMs()) {
                            const int64_t delta = now - *anchor;
                            if (delta >= 0 && delta < 60LL * 60 * 1000) {
                                positionBaseMs = delta;
                            }
                        }
                    }
                    positionWasPlaying = track.playing;
                } else if (track.hasMetadata()) {
                    if (track.playing != positionWasPlaying) {
                        if (!track.playing && positionWasPlaying) {
                            positionBaseMs += now - positionBaseAtMs;
                            positionBaseAtMs = now;
                        } else if (track.playing && !positionWasPlaying) {
                            positionBaseAtMs = now;
                        }
                        positionWasPlaying = track.playing;
                    }
                } else {
                    currentKey.clear();
                    artworkUrl.clear();
                    hasMetadata = false;
                    metadata = ArtworkService::Result{};
                    lastSentKey.clear();
                    lastSentArtwork.clear();
                }

                if (const auto resolved = artwork.takeResult()) {
                    if (!currentKey.empty() && resolved->key == currentKey) {
                        metadata = *resolved;
                        hasMetadata = true;
                        artworkUrl = resolved->url;
                        forceSend = true;
                        if (metadata.durationMs > 0 && positionBaseMs > metadata.durationMs) {
                            positionBaseMs = 0;
                            positionBaseAtMs = now;
                        }
                    }
                }

                TrackInfo display = track;
                bool wantActivity = false;
                bool seeked = false;
                if (track.hasMetadata()) {
                    if (hasMetadata) {
                        if (!metadata.album.empty()) display.album = metadata.album;
                        // Amazon Music sometimes reports the track title as the artist.
                        // Prefer iTunes' artist in that case (and when the artist is empty).
                        if (!metadata.artist.empty() &&
                            (display.artist.empty() || display.artist == display.title)) {
                            display.artist = metadata.artist;
                        }
                        if (display.durationMs <= 0) display.durationMs = metadata.durationMs;
                    }

                    int64_t position = positionBaseMs;
                    if (track.playing) position += now - positionBaseAtMs;
                    if (display.durationMs > 0 && position > display.durationMs) {
                        position = display.durationMs;
                    }
                    if (position < 0) position = 0;

                    // Prefer a real media-session timeline when an app provides one.
                    const bool hasTimeline = track.durationMs > 0 && track.positionMs > 0;
                    if (hasTimeline) {
                        position = track.positionMs;
                    }
                    display.positionMs = position;

                    if (hasTimeline && track.playing && lastSentAtMs > 0 && lastSentPlaying) {
                        const int64_t expected = lastSentPositionMs + (now - lastSentAtMs);
                        if (std::llabs(expected - position) > 2500) {
                            seeked = true;
                            positionBaseMs = position;
                            positionBaseAtMs = now;
                        }
                    }

                    wantActivity =
                        track.playing || (track.paused && config.showPaused);
                }

                if (!wantActivity) {
                    if (activityShown && discord.isConnected()) {
                        discord.clearActivity();
                        activityShown = false;
                    }
                    lastSentKey.clear();
                    lastSentArtwork.clear();
                    lastSentPlaying = false;
                    lastSentAtMs = 0;
                    forceSend = true;
                } else {
                    const bool changed = forceSend || currentKey != lastSentKey ||
                                         display.playing != lastSentPlaying ||
                                         artworkUrl != lastSentArtwork || seeked;

                    if (changed && discord.isConnected()) {
                        logDebug("アクティビティを送信します (key=" + currentKey + ")");
                        const std::string activity =
                            buildActivity(display, artworkUrl, config.statusDisplay);
                        if (discord.setActivity(activity)) {
                            logDebug("アクティビティを送信しました。");
                            const bool isNewTrackDisplay = currentKey != lastSentKey;
                            activityShown = true;
                            forceSend = false;
                            lastSentKey = currentKey;
                            lastSentArtwork = artworkUrl;
                            lastSentPlaying = display.playing;
                            lastSentAtMs = now;
                            lastSentPositionMs = display.positionMs;

                            if (isNewTrackDisplay) {
                                logInfo((display.playing ? "再生中: " : "一時停止: ") +
                                        formatTrackLine(display));
                            } else {
                                logDebug("プロフィールを更新しました。");
                            }
                        }
                    }
                }

                if (!discord.isConnected()) {
                    const auto steadyNow = std::chrono::steady_clock::now();
                    if (steadyNow - lastConnectAttempt >= std::chrono::seconds(5)) {
                        lastConnectAttempt = steadyNow;
                        if (discord.connect()) {
                            forceSend = true;
                            activityShown = false;
                            reportedDiscordError.clear();
                        } else {
                            const std::string connectionError = discord.lastError();
                            if (!connectionError.empty() &&
                                connectionError != reportedDiscordError) {
                                logError("Discord に接続できません: " + connectionError);
                                reportedDiscordError = connectionError;
                            }
                        }
                    }
                }

                status.discordConnected = discord.isConnected();
                if (!reportedDiscordError.empty()) {
                    status.discordError = reportedDiscordError;
                } else if (status.discordConnected) {
                    status.discordError.clear();
                }
                status.hasTrack = track.hasMetadata();
                if (track.hasMetadata()) {
                    status.playing = display.playing;
                    status.paused = display.paused;
                    status.title = display.title;
                    status.artist = display.artist;
                    status.album = display.album;
                    status.sourceApp = display.sourceApp;
                    status.artworkUrl = artworkUrl;
                    status.positionMs = display.positionMs;
                    status.durationMs = display.durationMs;
                    status.message = display.playing ? "再生中"
                                                   : (display.paused ? "一時停止" : "停止");
                } else {
                    status.playing = false;
                    status.paused = false;
                    status.title.clear();
                    status.artist.clear();
                    status.album.clear();
                    status.sourceApp.clear();
                    status.artworkUrl.clear();
                    status.positionMs = 0;
                    status.durationMs = 0;
                    status.message = "待機中";
                }
                publish(status);

                const int sleepTotal = std::max(200, config.pollMs);
                int slept = 0;
                while (slept < sleepTotal && !stop_.load()) {
                    const int slice = std::min(100, sleepTotal - slept);
                    std::this_thread::sleep_for(std::chrono::milliseconds(slice));
                    slept += slice;
                }
            }

            logInfo("終了しています...");
            if (discord.isConnected()) {
                discord.clearActivity();
            }
            discord.disconnect();
            artwork.stop();

            WorkerSnapshot stopped;
            stopped.rpcEnabled = true;
            stopped.message = "停止中";
            publish(stopped);
        }
    }

    if (apartmentInitialized) {
        try {
            winrt::uninit_apartment();
        } catch (...) {
        }
    }
}
