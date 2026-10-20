#include "amazon_cache.hpp"

#include "amazon_log.hpp"
#include "json.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

std::wstring localAppData() {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if (length == 0 || length >= 32768) return {};
    return std::wstring(buffer, length);
}

std::vector<std::wstring> cacheDirectories() {
    std::vector<std::wstring> result;
    const std::wstring local = localAppData();
    if (local.empty()) return result;

    const std::wstring packages = local + L"\\Packages";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(packages, error)) {
        if (error) break;
        std::error_code typeError;
        if (!entry.is_directory(typeError)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"AmazonMobileLLC.AmazonMusic", 0) != 0) continue;
        result.push_back(entry.path().wstring() +
                         L"\\LocalCache\\Local\\Amazon Music\\Data\\Local Storage");
    }
    result.push_back(local + L"\\Amazon Music\\Data\\Local Storage");
    return result;
}

struct CacheFile {
    std::wstring path;
    std::filesystem::file_time_type modified;
};

std::vector<CacheFile> listCacheFiles() {
    std::vector<CacheFile> result;
    for (const auto& directory : cacheDirectories()) {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) break;
            std::error_code typeError;
            if (!entry.is_regular_file(typeError)) continue;
            const std::wstring extension = entry.path().extension().wstring();
            if (extension != L".log" && extension != L".ldb") continue;
            std::error_code timeError;
            const auto modified = entry.last_write_time(timeError);
            if (timeError) continue;
            result.push_back({entry.path().wstring(), modified});
        }
    }

    std::sort(result.begin(), result.end(), [](const CacheFile& a, const CacheFile& b) {
        return a.modified > b.modified;
    });
    if (result.size() > 40) result.resize(40);
    return result;
}

bool readFile(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        return false;
    }

    constexpr int64_t kMaxRead = 16 * 1024 * 1024;
    const int64_t offset = std::max<int64_t>(0, size.QuadPart - kMaxRead);
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart - offset));
    size_t total = 0;
    while (total < out.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - total, 256 * 1024));
        if (!ReadFile(file, out.data() + total, chunk, &read, nullptr) || read == 0) break;
        total += read;
    }
    CloseHandle(file);
    out.resize(total);
    return !out.empty();
}

size_t findJsonEnd(const std::string& text, size_t start) {
    bool inString = false;
    bool escaped = false;
    int depth = 0;
    for (size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

std::optional<AmazonCachedTrack> parseTrack(const std::string& text,
                                           const std::string& expectedAsin) {
    const auto parsed = json::parse(text);
    if (!parsed || !parsed->isObject()) return std::nullopt;

    // The cache also contains large wrapper objects that include many tracks.
    // Only accept an object whose own ASIN (or uniqueId) matches the track we
    // are looking for.
    bool asinMatches = false;
    if (const auto* asin = parsed->find("asin"); asin && asin->isString()) {
        asinMatches = asin->asString() == expectedAsin;
    }
    if (!asinMatches) {
        if (const auto* uniqueId = parsed->find("uniqueId");
            uniqueId && uniqueId->isString()) {
            asinMatches = uniqueId->asString() == expectedAsin;
        }
    }
    if (!asinMatches) return std::nullopt;

    const auto* title = parsed->find("title");
    if (!title || !title->isString() || title->asString().empty()) {
        title = parsed->find("primaryTitle");
    }
    if (!title || !title->isString() || title->asString().empty()) return std::nullopt;

    AmazonCachedTrack track;
    track.asin = expectedAsin;
    track.title = title->asString();

    if (const auto* concatenated = parsed->find("concatenatedArtistName");
        concatenated && concatenated->isString()) {
        track.artist = concatenated->asString();
    }
    if (track.artist.empty()) {
        if (const auto* artist = parsed->find("artist"); artist && artist->isObject()) {
            if (const auto* name = artist->find("name"); name && name->isString()) {
                track.artist = name->asString();
            }
        }
    }
    if (track.artist.empty()) {
        if (const auto* artists = parsed->find("artists");
            artists && artists->isArray() && !artists->array.empty()) {
            const auto& first = artists->array.front();
            if (first.isObject()) {
                if (const auto* name = first.find("name"); name && name->isString()) {
                    track.artist = name->asString();
                }
            }
        }
    }

    if (const auto* album = parsed->find("album"); album && album->isObject()) {
        if (const auto* name = album->find("name"); name && name->isString()) {
            track.album = name->asString();
        } else if (const auto* albumTitle = album->find("title");
                   albumTitle && albumTitle->isString()) {
            track.album = albumTitle->asString();
        }
    }

    if (const auto* duration = parsed->find("duration"); duration) {
        if (duration->isString()) {
            try {
                track.durationMs = std::stoll(duration->asString()) * 1000;
            } catch (...) {
            }
        } else if (duration->type == json::Value::Type::Number) {
            track.durationMs = static_cast<int64_t>(duration->number * 1000.0);
        }
    }

    return track;
}

std::optional<AmazonCachedTrack> parseNear(const std::string& text, size_t asinPosition,
                                           const std::string& asin) {
    // The ASIN is near the end of the track object. Walk backwards through
    // enclosing '{' candidates and accept the first object that contains the
    // ASIN and whose own ASIN matches it.
    size_t cursor = asinPosition;
    for (int attempt = 0; attempt < 128 && cursor > 0; ++attempt) {
        const size_t start = text.rfind('{', cursor - 1);
        if (start == std::string::npos) break;
        cursor = start;

        const size_t end = findJsonEnd(text, start);
        if (end == std::string::npos || end < asinPosition) continue;
        if (auto track = parseTrack(text.substr(start, end - start + 1), asin)) {
            return track;
        }
    }
    return std::nullopt;
}

int trackScore(const AmazonCachedTrack& track) {
    int score = 0;
    if (!track.title.empty()) score += 4;
    if (!track.artist.empty()) score += 2;
    if (!track.album.empty()) score += 2;
    if (track.durationMs > 0) score += 1;
    return score;
}

std::mutex g_cacheMutex;
std::string g_cachedAsin;
std::optional<AmazonCachedTrack> g_cachedTrack;

} // namespace

std::optional<AmazonCachedTrack> findAmazonCachedTrack() {
    const std::vector<std::string> asins = lastAmazonTrackAsins();
    if (asins.empty()) return std::nullopt;

    const std::string& primaryAsin = asins.front();
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        if (g_cachedTrack && g_cachedAsin == primaryAsin) return g_cachedTrack;
    }

    const std::vector<CacheFile> files = listCacheFiles();
    for (const auto& asin : asins) {
        std::optional<AmazonCachedTrack> best;
        int bestScore = -1;

        for (const auto& file : files) {
            std::string text;
            if (!readFile(file.path, text)) continue;

            std::string needle = "\"uniqueId\":\"" + asin + "\"";
            size_t position = text.find(needle);
            if (position == std::string::npos) {
                needle = "\"asin\":\"" + asin + "\"";
                position = text.find(needle);
            }

            while (position != std::string::npos) {
                if (auto track = parseNear(text, position, asin)) {
                    const int score = trackScore(*track);
                    if (score > bestScore) {
                        bestScore = score;
                        best = std::move(track);
                    }
                }
                position = text.find(needle, position + needle.size());
            }

            // A full catalog entry has title, artist, album and duration.
            if (bestScore >= 9) break;
        }

        if (best) {
            if (bestScore >= 9) {
                std::lock_guard<std::mutex> lock(g_cacheMutex);
                g_cachedAsin = asin;
                g_cachedTrack = best;
            }
            return best;
        }
    }
    return std::nullopt;
}
