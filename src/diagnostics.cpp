#include "diagnostics.hpp"

#include "amazon_log.hpp"
#include "artwork.hpp"
#include "media.hpp"
#include "util.hpp"

#include <winrt/base.h>

#include <chrono>
#include <iostream>
#include <thread>

void printUsage() {
    std::cout
        << "Amazon Music Discord RPC\n\n"
        << "通常は引数なしで起動すると、通知領域に常駐します。\n"
        << "アイコンをクリックすると設定画面が開きます。\n\n"
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

int runProbe(const Config& config) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    int result = 0;
    {
        MediaWatcher media;
        std::string error;
        if (!media.initialize(config.appFilter, true, error)) {
            std::cout << error << "\n";
            result = 1;
        } else {
            std::cout << "\n=== メディアセッション一覧 ===\n";
            const auto sessions = media.listSessions();
            if (sessions.empty()) {
                std::cout << "メディアセッションが見つかりません。\n";
            }
            int index = 1;
            for (const auto& session : sessions) {
                std::cout << "[" << index++ << "] AppID: " << session.appId << "\n";
                std::cout << "    状態: " << (session.status.empty() ? "不明" : session.status)
                          << "\n";
                std::cout << "    曲名: " << (session.title.empty() ? "(なし)" : session.title)
                          << "\n";
                std::cout << "    アーティスト: "
                          << (session.artist.empty() ? "(なし)" : session.artist) << "\n\n";
            }

            std::cout << "=== フィルター \"" << config.appFilter << "\" の判定 ===\n";
            const TrackInfo track = media.poll();
            if (!track.hasMetadata()) {
                std::cout << "対象の再生情報を取得できませんでした。\n";
                std::cout << "Amazon Music で曲を再生した状態で試してください。\n";
                result = 2;
            } else {
                std::cout << "アプリ: " << track.sourceApp << "\n";
                std::cout << "状態: "
                          << (track.playing ? "再生中" : track.paused ? "一時停止" : "停止")
                          << "\n";
                std::cout << "タイトル: " << track.title << "\n";
                std::cout << "アーティスト: " << track.artist << "\n";
                std::cout << "アルバム: " << track.album << "\n";
                std::cout << "アルバムアーティスト: " << track.albumArtist << "\n";
                std::cout << "サブタイトル: " << track.subtitle << "\n";
                std::cout << "サムネイル: " << track.thumbnail.size() << " bytes ("
                          << track.thumbnailType << ")\n";
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
                    std::cout << "  " << i << "s: " << formatDuration(current.positionMs)
                              << " / " << formatDuration(current.durationMs)
                              << (current.playing ? " (playing)" : " (not playing)") << "\n";
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        }
    }
    winrt::uninit_apartment();
    return result;
}

int runLookup(const Config& config, const std::string& title, const std::string& artist) {
    TrackInfo track;
    track.hasSession = true;
    track.title = title;
    track.artist = artist;

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
