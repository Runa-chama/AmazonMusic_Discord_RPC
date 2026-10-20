#pragma once

#include <string>

struct Config {
    std::string clientId;
    std::string appFilter = "amazon";
    std::string artSource = "itunes"; // itunes | catbox | none
    std::string artCountry = "JP";
    // Which activity field Discord shows in the member list / status line:
    // details = song title, state = artist, name = Discord application name.
    std::string statusDisplay = "details"; // details | state | name
    int pollMs = 750;
    bool showPaused = true;
    bool rpcEnabled = true;
};

// Loads config.ini from the executable directory (or the given path).
// Creates a commented template when the file does not exist.
Config loadConfig(const std::wstring& path, bool& created);

bool saveConfig(const std::wstring& path, const Config& config);

bool isValidClientId(const std::string& clientId);
