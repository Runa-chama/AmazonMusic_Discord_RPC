#pragma once

#include <string>

struct Config {
    std::string clientId;
    std::string appFilter = "amazon";
    std::string artSource = "itunes"; // itunes | catbox | none
    std::string artCountry = "JP";
    int pollMs = 750;
    bool showPaused = true;
};

// Loads config.ini from the executable directory (or the given path).
// Creates a commented template when the file does not exist.
Config loadConfig(const std::wstring& path, bool& created);

bool saveConfig(const std::wstring& path, const Config& config);

bool isValidClientId(const std::string& clientId);
