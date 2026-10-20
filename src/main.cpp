#include "amazon_log.hpp"
#include "artwork.hpp"
#include "config.hpp"
#include "discord_ipc.hpp"
#include "json.hpp"
#include "media.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <winrt/base.h>

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

void printUsage() {
    std::cout
        << "Amazon Music Discord RPC\n\n"
        << "使い方:\n"
        << "  AmazonMusicRPC.exe [オプション]\n\n"
        << "オプション:\n"
        << "  --config <path>     設定ファイルのパスを指定\n"
        << "  --client-id <id>    Discord Application ID を一時的に指定\n"
        << "  --no-art            ジャケット画像を表示しない\n"
        << "  --any               対象アプリを問わず、再生中のメディアを表示\n"
        << "  --lookup <title> [artist]  iTunes メタデータ検索をテスト\n"
        << "  --probe             メディアセッションの検出結果だけを表示\n"
        << "  --verbose, -v       デバッグログを表示\n"
        << "  --help, -h          このヘルプを表示\n";
}

std::string buildActivity(const TrackInfo& track, const std::string& artworkUrl) {
    const std::string title = json::truncateUtf8(track.title, 128);
    std::string artist = track.artist;
    if (artist.empty() || artist == track.title) artist = "Amazon Music";
    artist = json::truncateUtf8(artist, 128);

    std::string activity = "{\"type\":2";
    activity += ",\"details\":" + json::quote(title);
    activity += ",\"state\":" + json::quote(artist);

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

int runProbe(MediaWatcher& media, const Config& config) {
    std::cout << "\n=== メディアセッション一覧 ===\n";
    const auto sessions = media.listSessions();
    if (sessions.empty()) {
        std::cout << "メディアセッションが見つかりません。\n";
    }
    int index = 1;
    for (const auto& session : sessions) {
        std::cout << "[" << index++ << "] AppID: " << session.appId << "\n";
        std::cout << "    状態: " << (session.status.empty() ? "不明" : session.status) << "\n";
        std::cout << "    曲名: " << (session.title.empty() ? "(なし)" : session.title) << "\n";
        std::cout << "    アーティスト: " << (session.artist.empty() ? "(なし)" : session.artist)
                  << "\n\n";
    }

    std::cout << "=== フィルター \"" << config.appFilter << "\" の判定 ===\n";
    const TrackInfo track = media.poll();
    if (!track.hasMetadata()) {
        std::cout << "対象の再生情報を取得できませんでした。\n";
        std::cout << "Amazon Music で曲を再生した状態で試してください。\n";
        return 2;
    }

    std::cout << "アプリ: " << track.sourceApp << "\n";
    std::cout << "状態: " << (track.playing ? "再生中" : track.paused ? "一時停止" : "停止")
              << "\n";
    std::cout << "タイトル: " << track.title << "\n";
    std::cout << "アーティスト: " << track.artist << "\n";
    std::cout << "アルバム: " << track.album << "\n";
    std::cout << "アルバムアーティスト: " << track.albumArtist << "\n";
    std::cout << "サブタイトル: " << track.subtitle << "\n";
    std::cout << "サムネイル: " << track.thumbnail.size() << " bytes (" << track.thumbnailType
              << ")\n";
    std::cout << "位置: " << formatDuration(track.positionMs) << " / "
              << formatDuration(track.durationMs) << "\n";

    if (const auto anchor = lastAmazonPlaybackTerminationUnixMs()) {
        const int64_t elapsed = unixNowMs() - *anchor;
        std::cout << "Amazon Music ログの前曲終了: " << elapsed / 1000
                  << " 秒前 (再生位置の推定に使用)\n";
    } else {
        std::cout << "Amazon Music ログの再生位置情報: 見つかりません\n";
    }

    std::cout << "\n=== 5 秒間のタイムライン監視 ===\n";
    for (int i = 0; i < 5; ++i) {
        const TrackInfo current = media.poll();
        std::cout << "  " << i << "s: " << formatDuration(current.positionMs) << " / "
                  << formatDuration(current.durationMs)
                  << (current.playing ? " (playing)" : " (not playing)") << "\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return 0;
}

int inputClientId(Config& config, const std::wstring& configPath) {
    std::cout << "\nDiscord Application ID を入力してください。\n"
              << "(Discord Developer Portal でアプリを作成し、Application ID をコピーします)\n"
              << "> " << std::flush;

    std::string value;
    if (!std::getline(std::cin, value)) return 1;
    value = trim(value);
    if (!isValidClientId(value)) {
        logError("Application ID は 17〜20 桁の数字です。");
        return 1;
    }

    config.clientId = value;
    if (!saveConfig(configPath, config)) {
        logError("config.ini に保存できませんでした。");
        return 1;
    }
    std::cout << "config.ini に保存しました。\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"Amazon Music Discord RPC");
    SetConsoleCtrlHandler(consoleHandler, TRUE);
    std::ios::sync_with_stdio(false);

    std::wstring configPath = executableDirectory() + L"\\config.ini";
    std::string clientIdOverride;
    bool probe = false;
    bool noArt = false;
    bool anyApp = false;
    bool doLookup = false;
    std::string lookupTitle;
    std::string lookupArtist;
    bool interactive = argc <= 1;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h") {
            printUsage();
            return 0;
        }
        if (arg == L"--probe") {
            probe = true;
        } else if (arg == L"--no-art") {
            noArt = true;
        } else if (arg == L"--any") {
            anyApp = true;
        } else if (arg == L"--lookup" && i + 1 < argc) {
            doLookup = true;
            lookupTitle = utf8FromWide(argv[++i]);
            if (i + 1 < argc) {
                const std::wstring next = argv[i + 1];
                if (next.rfind(L"--", 0) != 0) {
                    lookupArtist = utf8FromWide(argv[++i]);
                }
            }
        } else if (arg == L"--verbose" || arg == L"-v") {
            setVerbose(true);
        } else if (arg == L"--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == L"--client-id" && i + 1 < argc) {
            clientIdOverride = utf8FromWide(argv[++i]);
        } else {
            logError("不明な引数です: " + utf8FromWide(arg));
            printUsage();
            return 1;
        }
    }

    bool created = false;
    Config config = loadConfig(configPath, created);
    if (noArt) config.artSource = "none";
    if (anyApp) config.appFilter.clear();
    if (!clientIdOverride.empty()) config.clientId = clientIdOverride;

    if (doLookup) {
        TrackInfo track;
        track.hasSession = true;
        track.title = lookupTitle;
        track.artist = lookupArtist;

        std::cout << "iTunes 検索: " << track.title;
        if (!track.artist.empty()) std::cout << " / " << track.artist;
        std::cout << "\n\n";

        ArtworkService service("itunes", config.artCountry);
        const auto result = service.lookup(track);
        if (!result.hasMetadata) {
            std::cout << "一致する曲が見つかりませんでした。\n";
            return 2;
        }

        std::cout << "アルバム     : " << result.album << "\n";
        std::cout << "アーティスト : " << result.artist << "\n";
        std::cout << "長さ         : " << formatDuration(result.durationMs) << "\n";
        std::cout << "ジャケット   : " << result.url << "\n";
        return 0;
    }

    if (!isValidClientId(config.clientId) && !probe) {
        if (created) {
            std::cout << "config.ini を作成しました。\n\n"
                      << "Discord のプロフィールに表示するには、Discord Application ID が必要です。\n"
                      << "1. https://discord.com/developers/applications を開く\n"
                      << "2. \"New Application\" でアプリを作成 (名前がプロフィールに表示されます)\n"
                      << "3. \"Application ID\" をコピーする\n\n";
        }
        if (inputClientId(config, configPath) != 0) {
            if (interactive) {
                std::cout << "\nEnter キーで終了します..." << std::flush;
                std::cin.get();
            }
            return 1;
        }
    }

    if (probe) {
        std::cout << "Amazon Music Discord RPC - メディアセッション診断\n";
        std::cout << "設定ファイル: " + utf8FromWide(configPath) + "\n";
    } else {
        std::cout << "==============================================\n"
                  << " Amazon Music Discord RPC\n"
                  << "==============================================\n"
                  << "設定ファイル : " << utf8FromWide(configPath) << "\n"
                  << "Application ID: " << config.clientId << "\n"
                  << "対象アプリ   : " << config.appFilter << "\n"
                  << "ジャケット   : " << config.artSource << "\n"
                  << "終了するには Ctrl+C を押してください。\n\n";
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    MediaWatcher media;
    std::string error;
    if (!media.initialize(config.appFilter, probe, error)) {
        logError(error);
        if (interactive) {
            std::cout << "\nEnter キーで終了します..." << std::flush;
            std::cin.get();
        }
        return 1;
    }

    if (probe) {
        const int result = runProbe(media, config);
        if (interactive) {
            std::cout << "\nEnter キーで終了します..." << std::flush;
            std::cin.get();
        }
        return result;
    }

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

    logInfo("Amazon Music の再生を待機しています...");

    while (!g_stop.load()) {
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

        const bool wantActivity =
            track.hasMetadata() && (track.playing || (track.paused && config.showPaused));

        if (!wantActivity) {
            if (activityShown && discord.isConnected()) {
                discord.clearActivity();
                activityShown = false;
            }
            lastSentKey.clear();
            lastSentArtwork.clear();
            lastSentPlaying = false;
            forceSend = true;
        } else {
            TrackInfo display = track;
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

            bool seeked = false;
            if (hasTimeline && track.playing && lastSentAtMs > 0 && lastSentPlaying) {
                const int64_t expected = lastSentPositionMs + (now - lastSentAtMs);
                if (std::llabs(expected - position) > 2500) {
                    seeked = true;
                    positionBaseMs = position;
                    positionBaseAtMs = now;
                }
            }

            const bool changed = forceSend || currentKey != lastSentKey ||
                                 display.playing != lastSentPlaying ||
                                 artworkUrl != lastSentArtwork || seeked;

            if (changed && discord.isConnected()) {
                logDebug("アクティビティを送信します (key=" + currentKey + ")");
                const std::string activity = buildActivity(display, artworkUrl);
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
                    if (!connectionError.empty() && connectionError != reportedDiscordError) {
                        logError("Discord に接続できません: " + connectionError);
                        reportedDiscordError = connectionError;
                    }
                }
            }
        }

        const int sleepTotal = std::max(200, config.pollMs);
        int slept = 0;
        while (slept < sleepTotal && !g_stop.load()) {
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
    return 0;
}
