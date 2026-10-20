#include "artwork.hpp"

#include "http.hpp"
#include "json.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::string normalizeForMatch(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        if (c < 0x80) {
            if (c >= 'A' && c <= 'Z') {
                out.push_back(static_cast<char>(c - 'A' + 'a'));
            } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                out.push_back(static_cast<char>(c));
            }
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool containsMatch(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || haystack.empty()) return false;
    return haystack.find(needle) != std::string::npos;
}

std::string upgradeArtworkUrl(std::string url) {
    if (url.rfind("http://", 0) == 0) {
        url = "https://" + url.substr(7);
    }
    const std::string from = "100x100bb";
    const size_t pos = url.find(from);
    if (pos != std::string::npos) {
        url.replace(pos, from.size(), "600x600bb");
    }
    return url;
}

std::wstring localAppData() {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if (length == 0 || length >= 32768) return {};
    return std::wstring(buffer, length);
}

std::vector<std::filesystem::path> artworkCacheDirectories() {
    std::vector<std::filesystem::path> directories;
    const std::wstring local = localAppData();
    if (local.empty()) return directories;

    const std::filesystem::path packages = std::wstring(local + L"\\Packages");
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(packages, error)) {
        if (error) break;
        std::error_code typeError;
        if (!entry.is_directory(typeError)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"AmazonMobileLLC.AmazonMusic", 0) != 0) continue;

        directories.push_back(entry.path() / L"LocalCache" / L"Local" / L"Amazon Music" /
                              L"Data" / L"Artwork Cache");
    }

    directories.push_back(std::filesystem::path(local) / L"Amazon Music" / L"Data" /
                          L"Artwork Cache");
    return directories;
}

int64_t fileTimeToUnixMs(const FILETIME& fileTime) {
    const uint64_t ticks =
        (static_cast<uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
    return static_cast<int64_t>(ticks / 10000) - 11644473600000LL;
}

struct NotificationArt {
    std::vector<uint8_t> data;
    std::string extension;
};

std::optional<NotificationArt> findRecentNotificationArt(int64_t minimumMtimeMs) {
    std::optional<NotificationArt> newest;
    int64_t newestTime = minimumMtimeMs;

    for (const auto& directory : artworkCacheDirectories()) {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) continue;

        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) break;
            std::error_code typeError;
            if (!entry.is_regular_file(typeError)) continue;

            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"notification", 0) != 0) continue;

            std::wstring extension = entry.path().extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
            if (extension != L".jpg" && extension != L".jpeg" && extension != L".png") continue;

            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!GetFileAttributesExW(entry.path().c_str(), GetFileExInfoStandard, &attributes)) {
                continue;
            }
            const int64_t mtime = fileTimeToUnixMs(attributes.ftLastWriteTime);
            if (mtime <= newestTime) continue;

            HANDLE file = CreateFileW(entry.path().c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) continue;

            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 24 * 1024 * 1024) {
                CloseHandle(file);
                continue;
            }

            NotificationArt art;
            art.data.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            const BOOL ok = ReadFile(file, art.data.data(), static_cast<DWORD>(art.data.size()),
                                     &read, nullptr);
            CloseHandle(file);
            if (!ok || read == 0) continue;

            art.data.resize(read);
            art.extension = utf8FromWide(extension.substr(1));
            newest = std::move(art);
            newestTime = mtime;
        }
    }

    return newest;
}

std::string cachePath() {
    return utf8FromWide(executableDirectory() + L"\\artcache.txt");
}

std::string sanitizeKey(std::string key) {
    for (char& c : key) {
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    }
    return key;
}

} // namespace

ArtworkService::ArtworkService(std::string source, std::string country)
    : source_(std::move(source)), country_(std::move(country)) {
    loadUrlCache();
}

ArtworkService::~ArtworkService() {
    stop();
}

void ArtworkService::start() {
    if (worker_.joinable()) return;
    stop_ = false;
    worker_ = std::thread(&ArtworkService::loop, this);
}

void ArtworkService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    saveUrlCache();
}

void ArtworkService::request(uint64_t id, const TrackInfo& track) {
    if (track.title.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requestTrack_ = track;
        requestId_ = id;
        requestedAtMs_ = unixNowMs();
        hasRequest_ = true;
    }
    condition_.notify_one();
}

std::optional<ArtworkService::Result> ArtworkService::takeResult() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!result_) return std::nullopt;
    auto value = std::move(result_);
    result_.reset();
    return value;
}

ArtworkService::Result ArtworkService::lookup(const TrackInfo& track) {
    return resolve(track, unixNowMs());
}

void ArtworkService::loop() {
    while (true) {
        TrackInfo track;
        uint64_t id = 0;
        int64_t requestedAt = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stop_ || hasRequest_; });
            if (stop_) return;
            track = requestTrack_;
            id = requestId_;
            requestedAt = requestedAtMs_;
            hasRequest_ = false;
        }

        Result resolved = resolve(track, requestedAt);
        resolved.id = id;
        resolved.key = track.key();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (resolved.hasMetadata) {
                metadataCache_[resolved.key] = resolved;
            }
            if (!resolved.url.empty()) {
                urlCache_[resolved.key] = resolved.url;
            }
            result_ = resolved;
        }
    }
}

ArtworkService::Result ArtworkService::resolve(const TrackInfo& track, int64_t requestedAtMs) {
    Result result;
    result.key = track.key();
    const bool wantArtwork = source_ != "none";

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto cached = metadataCache_.find(track.key());
        if (cached != metadataCache_.end()) {
            result = cached->second;
            result.key = track.key();
            if (!wantArtwork) {
                result.url.clear();
            } else if (result.url.empty()) {
                const auto url = urlCache_.find(track.key());
                if (url != urlCache_.end()) result.url = url->second;
            }
            return result;
        }
    }

    // iTunes provides album, duration and (normally) artwork for the track.
    const ItunesInfo itunes = resolveItunes(track);
    if (itunes.found) {
        result.album = itunes.album;
        result.artist = itunes.artist;
        result.durationMs = itunes.durationMs;
        result.hasMetadata = true;
    }

    if (wantArtwork && result.url.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto url = urlCache_.find(track.key());
        if (url != urlCache_.end()) result.url = url->second;
    }

    if (source_ == "catbox" && result.url.empty()) {
        result.url = resolveNotificationArtwork(requestedAtMs - 2000);
    }

    if (wantArtwork && result.url.empty()) {
        result.url = itunes.artworkUrl;
    }

    return result;
}

ArtworkService::ItunesInfo ArtworkService::resolveItunes(const TrackInfo& track) {
    ItunesInfo info;

    std::string term = track.artist;
    if (!term.empty() && !track.title.empty()) term += " ";
    term += track.title;
    if (term.empty()) return info;

    const std::string url = "https://itunes.apple.com/search?term=" + urlEncode(term) +
                            "&media=music&entity=song&limit=10&country=" +
                            urlEncode(country_);
    logDebug("iTunes 検索: " + term);
    const auto response = httpGet(url);
    if (!response || response->statusCode != 200) {
        logDebug("iTunes 検索に失敗しました。");
        return info;
    }

    const auto parsed = json::parse(response->body);
    if (!parsed) return info;
    const auto* results = parsed->find("results");
    if (!results || !results->isArray()) return info;

    const std::string normalizedTitle = normalizeForMatch(track.title);
    const std::string normalizedArtist = normalizeForMatch(track.artist);
    const std::string normalizedAlbum = normalizeForMatch(track.album);

    int bestScore = 0;
    for (const auto& item : results->array) {
        const auto* trackName = item.find("trackName");
        if (!trackName || !trackName->isString()) continue;

        const std::string candidateTitle = normalizeForMatch(trackName->asString());
        if (candidateTitle.empty()) continue;

        int score = 0;
        if (candidateTitle == normalizedTitle) {
            score += 100;
        } else if (containsMatch(candidateTitle, normalizedTitle) ||
                   containsMatch(normalizedTitle, candidateTitle)) {
            score += 55;
        } else {
            continue;
        }

        const auto* artistName = item.find("artistName");
        if (artistName && artistName->isString() && !normalizedArtist.empty()) {
            const std::string candidateArtist = normalizeForMatch(artistName->asString());
            if (candidateArtist == normalizedArtist) {
                score += 80;
            } else if (containsMatch(candidateArtist, normalizedArtist) ||
                       containsMatch(normalizedArtist, candidateArtist)) {
                score += 40;
            }
        }

        const auto* collectionName = item.find("collectionName");
        if (collectionName && collectionName->isString() && !normalizedAlbum.empty()) {
            const std::string candidateAlbum = normalizeForMatch(collectionName->asString());
            if (candidateAlbum == normalizedAlbum) {
                score += 40;
            } else if (containsMatch(candidateAlbum, normalizedAlbum) ||
                       containsMatch(normalizedAlbum, candidateAlbum)) {
                score += 20;
            }
        } else if (collectionName && collectionName->isString()) {
            // When the player does not expose the album name, prefer a real
            // release title over an entry that is just the artist's name.
            const std::string candidateAlbum = normalizeForMatch(collectionName->asString());
            const std::string candidateArtist =
                artistName && artistName->isString() ? normalizeForMatch(artistName->asString())
                                                     : std::string();
            if (!candidateAlbum.empty() && candidateAlbum != candidateArtist) {
                score += 10;
            }
        }

        if (score <= bestScore) continue;
        bestScore = score;

        info = ItunesInfo{};
        info.found = true;
        const auto* artwork = item.find("artworkUrl100");
        if (artwork && artwork->isString()) {
            info.artworkUrl = upgradeArtworkUrl(artwork->asString());
        }
        if (collectionName && collectionName->isString()) {
            info.album = collectionName->asString();
        }
        if (artistName && artistName->isString()) {
            info.artist = artistName->asString();
        }
        const auto* duration = item.find("trackTimeMillis");
        if (duration && duration->type == json::Value::Type::Number) {
            info.durationMs = static_cast<int64_t>(duration->number);
        }
    }

    if (!info.found) {
        logDebug("iTunes でメタデータが見つかりませんでした。");
    }
    return info;
}

std::string ArtworkService::resolveNotificationArtwork(int64_t minimumMtimeMs) {
    // The Windows notification image is generated shortly after a track change.
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (stop_) return {};
        if (const auto art = findRecentNotificationArt(minimumMtimeMs)) {
            std::string extension = art->extension;
            std::string mime = "image/jpeg";
            if (extension == "png") mime = "image/png";
            else if (extension == "webp") mime = "image/webp";
            else if (extension == "gif") mime = "image/gif";
            else extension = "jpg";

            logDebug("Amazon Music の通知画像をアップロードしています...");
            const auto response = httpPostMultipart("https://catbox.moe/user/api.php",
                                                    "reqtype", "fileupload",
                                                    "cover." + extension, mime, art->data);
            if (response && response->statusCode == 200) {
                const std::string url = trim(response->body);
                if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0) {
                    return url;
                }
                logDebug("catbox の応答が不正です: " + url);
            } else {
                logDebug("catbox へのアップロードに失敗しました。");
            }
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    logDebug("Amazon Music の通知画像が見つかりませんでした。");
    return {};
}

void ArtworkService::loadUrlCache() {
    std::ifstream file(wideFromUtf8(cachePath()), std::ios::binary);
    if (!file) return;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string key = line.substr(0, tab);
        const std::string url = line.substr(tab + 1);
        if (!key.empty() && !url.empty()) urlCache_[key] = url;
    }
}

void ArtworkService::saveUrlCache() {
    std::ofstream file(wideFromUtf8(cachePath()), std::ios::binary | std::ios::trunc);
    if (!file) return;
    for (const auto& [key, url] : urlCache_) {
        file << sanitizeKey(key) << '\t' << url << "\r\n";
    }
}
