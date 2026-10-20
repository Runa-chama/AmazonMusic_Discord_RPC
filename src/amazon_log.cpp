#include "amazon_log.hpp"

#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::wstring localAppData() {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, 32768);
    if (length == 0 || length >= 32768) return {};
    return std::wstring(buffer, length);
}

std::vector<std::wstring> candidateLogPaths() {
    std::vector<std::wstring> paths;
    const std::wstring local = localAppData();
    if (local.empty()) return paths;

    const std::wstring packages = local + L"\\Packages";
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(packages, error)) {
        if (error) break;
        std::error_code typeError;
        if (!entry.is_directory(typeError)) continue;
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"AmazonMobileLLC.AmazonMusic", 0) != 0) continue;

        paths.push_back(entry.path().wstring() +
                        L"\\LocalCache\\Local\\Amazon Music\\Logs\\AmazonMusic.log");
    }

    paths.push_back(local + L"\\Amazon Music\\Logs\\AmazonMusic.log");
    return paths;
}

bool readTail(const std::wstring& path, std::string& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        return false;
    }

    constexpr int64_t kReadSize = 1024 * 1024;
    const int64_t offset = std::max<int64_t>(0, size.QuadPart - kReadSize);
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart - offset));
    DWORD total = 0;
    while (total < out.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<size_t>(out.size() - total, 256 * 1024));
        if (!ReadFile(file, out.data() + total, chunk, &read, nullptr) || read == 0) break;
        total += read;
    }
    CloseHandle(file);
    out.resize(total);
    return !out.empty();
}

std::optional<int64_t> parseLogTimestamp(const std::string& line) {
    // Example: "261020:161727      Browser INFO ..."
    if (line.size() < 13) return std::nullopt;
    for (int i = 0; i < 13; ++i) {
        if (i == 6) {
            if (line[i] != ':') return std::nullopt;
        } else if (line[i] < '0' || line[i] > '9') {
            return std::nullopt;
        }
    }

    const int year = 2000 + (line[0] - '0') * 10 + (line[1] - '0');
    const int month = (line[2] - '0') * 10 + (line[3] - '0');
    const int day = (line[4] - '0') * 10 + (line[5] - '0');
    const int hour = (line[7] - '0') * 10 + (line[8] - '0');
    const int minute = (line[9] - '0') * 10 + (line[10] - '0');
    const int second = (line[11] - '0') * 10 + (line[12] - '0');
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 59) {
        return std::nullopt;
    }

    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = static_cast<WORD>(day);
    st.wHour = static_cast<WORD>(hour);
    st.wMinute = static_cast<WORD>(minute);
    st.wSecond = static_cast<WORD>(second);

    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return std::nullopt;
    const uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(ticks / 10000) - 11644473600000LL;
}

} // namespace

std::optional<int64_t> lastAmazonPlaybackTerminationUnixMs() {
    for (const auto& path : candidateLogPaths()) {
        std::string text;
        if (!readTail(path, text)) continue;

        const std::string marker = "playbackTerminatedEvent";
        const size_t markerPos = text.rfind(marker);
        if (markerPos == std::string::npos) continue;

        const size_t lineStart = text.rfind('\n', markerPos);
        const size_t begin = lineStart == std::string::npos ? 0 : lineStart + 1;
        if (const auto timestamp = parseLogTimestamp(text.substr(begin, 13))) {
            return timestamp;
        }
    }
    return std::nullopt;
}

namespace {

std::string parseAsin(std::string_view text) {
    if (text.size() < 10) return {};
    for (size_t i = 0; i + 10 <= text.size(); ++i) {
        bool valid = true;
        for (size_t j = 0; j < 10; ++j) {
            const char c = text[i + j];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                valid = false;
                break;
            }
        }
        if (valid) return std::string(text.substr(i, 10));
    }
    return {};
}

} // namespace

std::vector<std::string> lastAmazonTrackAsins() {
    for (const auto& path : candidateLogPaths()) {
        std::string text;
        if (!readTail(path, text)) continue;

        size_t marker = text.rfind("StartingPlayback");
        if (marker == std::string::npos) marker = text.rfind("play called with uris:");
        if (marker == std::string::npos) marker = text.rfind("Playables : UriList =");
        if (marker == std::string::npos) continue;

        const size_t lineEnd = text.find('\n', marker);
        const size_t end = lineEnd == std::string::npos ? text.size() : lineEnd;
        const std::string_view line(text.data() + marker, end - marker);

        std::string requested;
        size_t asinPos = line.find("asin-//");
        if (asinPos == std::string_view::npos) {
            asinPos = line.find("asin://");
        }
        if (asinPos != std::string_view::npos) {
            requested = parseAsin(line.substr(asinPos + 7));
        }

        // Amazon may substitute an unavailable track with another catalog
        // entry. The substitute is what is actually playing, so try it first.
        std::vector<std::string> result;
        const size_t searchEnd = std::min(text.size(), marker + 8000);
        const std::string substituteMarker = "Substitute ASIN: ";
        size_t substitute = text.find(substituteMarker, marker);
        while (substitute != std::string::npos && substitute < searchEnd) {
            const std::string asin =
                parseAsin(std::string_view(text).substr(substitute + substituteMarker.size()));
            if (!asin.empty() &&
                std::find(result.begin(), result.end(), asin) == result.end()) {
                result.push_back(asin);
            }
            substitute = text.find(substituteMarker, substitute + 1);
        }
        if (!requested.empty() &&
            std::find(result.begin(), result.end(), requested) == result.end()) {
            result.push_back(requested);
        }

        if (!result.empty()) return result;
    }
    return {};
}
