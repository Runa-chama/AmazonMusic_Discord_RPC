#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

std::string utf8FromWide(const std::wstring& text);
std::wstring wideFromUtf8(const std::string& text);

std::string trim(std::string text);
std::string toLowerAscii(std::string text);
std::vector<std::string> split(const std::string& text, char delimiter);

std::wstring executableDirectory();
std::wstring executablePath();

int64_t unixNowMs();
std::string formatDuration(int64_t milliseconds);

void setVerbose(bool enabled);
bool verbose();
// Appends log lines to the given file (in addition to stdout). Rotates the
// previous file to <path>.old when it grows beyond a couple of megabytes.
void setLogFile(const std::wstring& path);
void logInfo(const std::string& message);
void logDebug(const std::string& message);
void logError(const std::string& message);
