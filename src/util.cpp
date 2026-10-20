#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <vector>

namespace {
std::atomic<bool> g_verbose{false};
std::mutex g_logMutex;

std::string timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

void writeLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << "[" << timestamp() << "] " << line << std::endl;
}
} // namespace

std::string utf8FromWide(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::wstring wideFromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string trim(std::string text) {
    const auto isSpace = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    size_t begin = 0;
    while (begin < text.size() && isSpace(static_cast<unsigned char>(text[begin]))) ++begin;
    size_t end = text.size();
    while (end > begin && isSpace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::string toLowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t pos = text.find(delimiter, start);
        if (pos == std::string::npos) {
            result.push_back(text.substr(start));
            break;
        }
        result.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return result;
}

std::wstring executablePath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::wstring(buffer.data(), length);
}

std::wstring executableDirectory() {
    std::wstring path = executablePath();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return path.substr(0, slash);
}

int64_t unixNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string formatDuration(int64_t milliseconds) {
    if (milliseconds < 0) milliseconds = 0;
    const int64_t totalSeconds = milliseconds / 1000;
    const int64_t hours = totalSeconds / 3600;
    const int64_t minutes = (totalSeconds % 3600) / 60;
    const int64_t seconds = totalSeconds % 60;
    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld",
                      static_cast<long long>(hours), static_cast<long long>(minutes),
                      static_cast<long long>(seconds));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", static_cast<long long>(minutes),
                      static_cast<long long>(seconds));
    }
    return buffer;
}

void setVerbose(bool enabled) {
    g_verbose.store(enabled);
}

bool verbose() {
    return g_verbose.load();
}

void logInfo(const std::string& message) {
    writeLine(message);
}

void logDebug(const std::string& message) {
    if (g_verbose.load()) writeLine(message);
}

void logError(const std::string& message) {
    writeLine("ERROR: " + message);
}
