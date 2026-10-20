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
void logInfo(const std::string& message);
void logDebug(const std::string& message);
void logError(const std::string& message);
