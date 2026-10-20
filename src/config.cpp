#include "config.hpp"

#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

const char* kDefaultConfig = "# Amazon Music Discord RPC\r\n"
                             "#\r\n"
                             "# Discord Developer Portal (https://discord.com/developers/applications)\r\n"
                             "# でアプリを作成し、Application ID を下に貼り付けてください。\r\n"
                             "# アプリ名が Discord プロフィールに表示される名前になります。\r\n"
                             "client_id=\r\n"
                             "\r\n"
                             "# 対象アプリの判定 (セミコロン区切り / 大文字小文字を無視した部分一致)\r\n"
                             "# Amazon Music アプリ: amazon\r\n"
                             "# ブラウザ版を使う場合: chrome;msedge;firefox\r\n"
                             "app_filter=amazon\r\n"
                             "\r\n"
                             "# ジャケット画像の取得方法\r\n"
                             "#   itunes : iTunes Search API から曲名/アーティストで検索 (アップロードなし)\r\n"
                             "#   catbox : Amazon Music のサムネイルを catbox.moe にアップロード (完全一致)\r\n"
                             "#   none   : 画像なし\r\n"
                             "art_source=itunes\r\n"
                             "\r\n"
                             "# iTunes 検索のストア (JP, US など)\r\n"
                             "art_country=JP\r\n"
                             "\r\n"
                             "# 名前の下のステータスに表示する項目\r\n"
                             "#   details : 曲名 (おすすめ)\r\n"
                             "#   state   : アーティスト\r\n"
                             "#   name    : Discord アプリ名\r\n"
                             "status_display=details\r\n"
                             "\r\n"
                             "# メディア情報のポーリング間隔 (ミリ秒)\r\n"
                             "poll_ms=750\r\n"
                             "\r\n"
                             "# 一時停止中もプロフィールに表示する\r\n"
                             "show_paused=true\r\n"
                             "\r\n"
                             "# Discord Rich Presence を有効にする\r\n"
                             "rpc_enabled=true\r\n";

std::string readFile(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

bool writeFile(const std::wstring& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(file);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool parseBool(const std::string& value, bool fallback) {
    const std::string v = lower(trim(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

std::string normalizeStatusDisplay(const std::string& value) {
    const std::string v = lower(trim(value));
    if (v == "state" || v == "name" || v == "details") return v;
    return "details";
}

} // namespace

Config loadConfig(const std::wstring& path, bool& created) {
    created = false;
    Config config;

    std::string text = readFile(path);
    if (text.empty()) {
        writeFile(path, kDefaultConfig);
        created = true;
        return config;
    }

    // Strip a UTF-8 BOM if present.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;

        const std::string key = lower(trim(line.substr(0, equals)));
        const std::string value = trim(line.substr(equals + 1));

        if (key == "client_id") {
            config.clientId = value;
        } else if (key == "app_filter") {
            config.appFilter = value;
        } else if (key == "art_source") {
            const std::string v = lower(value);
            if (v == "itunes" || v == "catbox" || v == "none") config.artSource = v;
        } else if (key == "art_country") {
            if (!value.empty()) config.artCountry = value;
        } else if (key == "status_display") {
            config.statusDisplay = normalizeStatusDisplay(value);
        } else if (key == "poll_ms") {
            try {
                config.pollMs = std::clamp(std::stoi(value), 250, 5000);
            } catch (...) {
            }
        } else if (key == "show_paused") {
            config.showPaused = parseBool(value, config.showPaused);
        } else if (key == "rpc_enabled") {
            config.rpcEnabled = parseBool(value, config.rpcEnabled);
        }
    }

    return config;
}

bool saveConfig(const std::wstring& path, const Config& config) {
    std::ostringstream out;
    out << "# Amazon Music Discord RPC\r\n";
    out << "# Application ID (Discord Developer Portal で作成したアプリの ID)\r\n";
    out << "client_id=" << config.clientId << "\r\n";
    out << "\r\n";
    out << "# 対象アプリ (セミコロン区切り / 部分一致 / 空欄で全て)\r\n";
    out << "app_filter=" << config.appFilter << "\r\n";
    out << "\r\n";
    out << "# ジャケット画像: itunes | catbox | none\r\n";
    out << "art_source=" << config.artSource << "\r\n";
    out << "art_country=" << config.artCountry << "\r\n";
    out << "# 名前の下のステータス: details (曲名) | state (アーティスト) | name (アプリ名)\r\n";
    out << "status_display=" << normalizeStatusDisplay(config.statusDisplay) << "\r\n";
    out << "poll_ms=" << config.pollMs << "\r\n";
    out << "show_paused=" << (config.showPaused ? "true" : "false") << "\r\n";
    out << "rpc_enabled=" << (config.rpcEnabled ? "true" : "false") << "\r\n";
    return writeFile(path, out.str());
}

bool isValidClientId(const std::string& clientId) {
    if (clientId.size() < 17 || clientId.size() > 20) return false;
    return std::all_of(clientId.begin(), clientId.end(), [](unsigned char c) {
        return c >= '0' && c <= '9';
    });
}
