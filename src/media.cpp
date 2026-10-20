#include "media.hpp"

#include "amazon_cache.hpp"
#include "util.hpp"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>

using namespace winrt;
using namespace winrt::Windows::Media::Control;

namespace {

int64_t dateTimeToUnixMs(winrt::Windows::Foundation::DateTime value) {
    // DateTime counts 100 ns intervals since 1601-01-01.
    constexpr int64_t kEpochDifferenceMs = 11644473600000LL;
    return static_cast<int64_t>(value.time_since_epoch().count() / 10000) - kEpochDifferenceMs;
}

bool isAmazonMusicApp(const std::string& appId) {
    const std::string lower = toLowerAscii(appId);
    return lower.find("amazonmusic") != std::string::npos ||
           lower.find("amazon music") != std::string::npos;
}

std::string detectImageType(const std::vector<uint8_t>& data) {
    if (data.size() >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return "png";
    }
    if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "jpg";
    }
    if (data.size() >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
        return "gif";
    }
    if (data.size() >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' &&
        data[3] == 'F' && data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return "webp";
    }
    return "bin";
}

std::vector<uint8_t> readThumbnail(const GlobalSystemMediaTransportControlsSessionMediaProperties& props) {
    std::vector<uint8_t> result;
    try {
        const auto reference = props.Thumbnail();
        if (!reference) return result;

        const auto stream = reference.OpenReadAsync().get();
        const auto size = stream.Size();
        if (size == 0 || size > 16 * 1024 * 1024) return result;

        winrt::Windows::Storage::Streams::DataReader reader(stream);
        reader.LoadAsync(static_cast<uint32_t>(size)).get();
        result.resize(static_cast<size_t>(size));
        reader.ReadBytes(winrt::array_view<uint8_t>(result.data(), result.data() + result.size()));
        reader.Close();
        stream.Close();
    } catch (const hresult_error&) {
        result.clear();
    }
    return result;
}

} // namespace

MediaWatcher::MediaWatcher() = default;

MediaWatcher::~MediaWatcher() = default;

bool MediaWatcher::initialize(const std::string& appFilter, bool needThumbnail,
                              std::string& error) {
    needThumbnail_ = needThumbnail;
    filters_.clear();
    for (auto& item : split(appFilter, ';')) {
        const std::string value = toLowerAscii(trim(item));
        if (!value.empty()) filters_.push_back(value);
    }

    try {
        manager_ = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    } catch (const hresult_error& ex) {
        error = "メディアセッションを取得できませんでした: " + utf8FromWide(ex.message().c_str());
        return false;
    }

    if (!manager_) {
        error = "メディアセッション マネージャーが利用できません。";
        return false;
    }
    return true;
}

bool MediaWatcher::matchesFilter(const std::string& appId) const {
    if (filters_.empty()) return true;
    const std::string lower = toLowerAscii(appId);
    for (const auto& filter : filters_) {
        if (lower.find(filter) != std::string::npos) return true;
    }
    return false;
}

std::string MediaWatcher::statusName(
    GlobalSystemMediaTransportControlsSessionPlaybackStatus status) {
    switch (status) {
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing: return "playing";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused: return "paused";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped: return "stopped";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed: return "closed";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened: return "opened";
    case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing: return "changing";
    default: return "unknown";
    }
}

TrackInfo MediaWatcher::poll() {
    TrackInfo info;
    if (!manager_) return info;

    GlobalSystemMediaTransportControlsSession session{nullptr};
    GlobalSystemMediaTransportControlsSessionPlaybackStatus status =
        GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed;
    int bestScore = -1;

    try {
        const auto sessions = manager_.GetSessions();
        for (uint32_t index = 0; index < sessions.Size(); ++index) {
            const auto candidate = sessions.GetAt(index);
            std::string appId;
            try {
                appId = winrt::to_string(candidate.SourceAppUserModelId());
            } catch (const hresult_error&) {
                continue;
            }
            if (!matchesFilter(appId)) continue;

            GlobalSystemMediaTransportControlsSessionPlaybackStatus candidateStatus =
                GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed;
            try {
                candidateStatus = candidate.GetPlaybackInfo().PlaybackStatus();
            } catch (const hresult_error&) {
                continue;
            }

            int score = 0;
            switch (candidateStatus) {
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing: score = 4; break;
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused: score = 3; break;
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Opened: score = 2; break;
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing: score = 2; break;
            case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped: score = 1; break;
            default: score = 0; break;
            }
            if (score > bestScore) {
                bestScore = score;
                session = candidate;
                status = candidateStatus;
            }
        }
    } catch (const hresult_error&) {
        return info;
    }

    if (!session || status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed) {
        lastKey_.clear();
        lastInfo_ = TrackInfo{};
        return info;
    }

    info.hasSession = true;
    info.playing = status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
    info.paused = status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused;
    try {
        info.sourceApp = winrt::to_string(session.SourceAppUserModelId());
    } catch (const hresult_error&) {
    }

    try {
        const auto props = session.TryGetMediaPropertiesAsync().get();
        info.title = winrt::to_string(props.Title());
        info.artist = winrt::to_string(props.Artist());
        info.album = winrt::to_string(props.AlbumTitle());
        info.albumArtist = winrt::to_string(props.AlbumArtist());
        info.subtitle = winrt::to_string(props.Subtitle());

        if (needThumbnail_) {
            const std::string key = info.key();
            if (key != lastKey_) {
                info.thumbnail = readThumbnail(props);
                info.thumbnailType = detectImageType(info.thumbnail);
            }
        }
    } catch (const hresult_error&) {
    }

    if (info.title.empty() && (info.playing || info.paused) &&
        isAmazonMusicApp(info.sourceApp)) {
        const int64_t now = unixNowMs();

        // Amazon Music's local catalog cache contains the full track metadata
        // and works while the app is minimized. Unlike screen capture, it does
        // not touch the Amazon Music window at all.
        if (now - lastCacheAttemptMs_ >= 1500) {
            lastCacheAttemptMs_ = now;
            if (const auto cachedTrack = findAmazonCachedTrack()) {
                info.title = cachedTrack->title;
                info.artist = cachedTrack->artist;
                info.album = cachedTrack->album;
                info.durationMs = cachedTrack->durationMs;
                if (info.key() != lastCacheKey_) {
                    lastCacheKey_ = info.key();
                    std::string line = info.title;
                    if (!info.artist.empty()) line += " — " + info.artist;
                    logInfo("Amazon Music のキャッシュから曲情報を取得しました: " + line);
                }
            }
        }
    }

    if (info.title.empty()) {
        // Keep the previous metadata if SMTC is empty and the cache has not
        // caught up yet. This also prevents flickering between empty polls.
        if (!lastInfo_.hasMetadata()) {
            info.hasSession = false;
            lastInfo_ = info;
            return info;
        }
        info.title = lastInfo_.title;
        info.artist = lastInfo_.artist;
        info.album = lastInfo_.album;
        info.albumArtist = lastInfo_.albumArtist;
        info.subtitle = lastInfo_.subtitle;
        info.durationMs = lastInfo_.durationMs;
    }

    try {
        const auto timeline = session.GetTimelineProperties();
        info.positionMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(timeline.Position()).count();
        const int64_t timelineDuration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                timeline.EndTime() - timeline.StartTime())
                .count();
        // Keep the duration recovered from Amazon's cache when SMTC reports no
        // usable timeline.
        if (timelineDuration > 0) info.durationMs = timelineDuration;

        if (info.playing && info.durationMs > 0) {
            const int64_t lastUpdated = dateTimeToUnixMs(timeline.LastUpdatedTime());
            const int64_t now = unixNowMs();
            if (lastUpdated > 0 && now > lastUpdated && now - lastUpdated < 60 * 60 * 1000) {
                info.positionMs += now - lastUpdated;
            }
        }

        if (info.positionMs < 0) info.positionMs = 0;
        if (info.durationMs > 0 && info.positionMs > info.durationMs) {
            info.positionMs = info.durationMs;
        }
    } catch (const hresult_error&) {
    }

    lastKey_ = info.key();
    lastInfo_ = info;
    return info;
}

std::vector<MediaWatcher::SessionSummary> MediaWatcher::listSessions() {
    std::vector<SessionSummary> result;
    if (!manager_) return result;

    try {
        const auto sessions = manager_.GetSessions();
        for (uint32_t index = 0; index < sessions.Size(); ++index) {
            const auto session = sessions.GetAt(index);
            SessionSummary summary;
            try {
                summary.appId = winrt::to_string(session.SourceAppUserModelId());
            } catch (const hresult_error&) {
            }
            try {
                const auto playback = session.GetPlaybackInfo();
                summary.status = statusName(playback.PlaybackStatus());
            } catch (const hresult_error&) {
            }
            try {
                const auto props = session.TryGetMediaPropertiesAsync().get();
                summary.title = winrt::to_string(props.Title());
                summary.artist = winrt::to_string(props.Artist());
            } catch (const hresult_error&) {
            }
            result.push_back(std::move(summary));
        }
    } catch (const hresult_error&) {
    }
    return result;
}
