#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include "config.hpp"
#include "diagnostics.hpp"
#include "util.hpp"
#include "worker.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <iostream>
#include <string>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kTrayClassName[] = L"AmazonMusicRpcTrayWindow";
constexpr wchar_t kSettingsClassName[] = L"AmazonMusicRpcSettingsWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kShowSettingsMessage = WM_APP + 2;
constexpr UINT_PTR kTrayTimer = 1;
constexpr UINT kTrayIconId = 1;
constexpr int kAppIconId = 101;

constexpr int IDC_CLIENT_ID = 1001;
constexpr int IDC_APP_FILTER = 1002;
constexpr int IDC_ART_SOURCE = 1003;
constexpr int IDC_ART_COUNTRY = 1004;
constexpr int IDC_STATUS_DISPLAY = 1005;
constexpr int IDC_POLL_MS = 1006;
constexpr int IDC_SHOW_PAUSED = 1007;
constexpr int IDC_RPC_ENABLED = 1008;
constexpr int IDC_START_WITH_WINDOWS = 1009;
constexpr int IDC_OPEN_PORTAL = 1010;
constexpr int IDC_SAVE = 1011;
constexpr int IDC_CANCEL = 1012;
constexpr int IDC_EXIT = 1013;
constexpr int IDC_OPEN_LOG = 1014;
constexpr int IDC_OPEN_FOLDER = 1015;
constexpr int IDC_STATUS_DISCORD = 1016;
constexpr int IDC_STATUS_TRACK = 1017;
constexpr int IDC_STATUS_RPC = 1018;
constexpr int IDC_HINT = 1019;

constexpr int ID_TRAY_SETTINGS = 2001;
constexpr int ID_TRAY_TOGGLE = 2002;
constexpr int ID_TRAY_EXIT = 2003;

HINSTANCE g_instance = nullptr;
HWND g_trayWindow = nullptr;
HWND g_settingsWindow = nullptr;
bool g_trayAdded = false;
bool g_balloonShown = false;
bool g_quitting = false;
UINT g_taskbarCreatedMessage = 0;
NOTIFYICONDATAW g_nid{};
UINT g_dpi = 96;
HFONT g_font = nullptr;
HFONT g_fontBold = nullptr;
HFONT g_fontTitle = nullptr;
HFONT g_fontSmall = nullptr;
HANDLE g_instanceMutex = nullptr;

Config g_config;
std::wstring g_configPath;
std::wstring g_logPath;
RpcWorker g_worker;

HWND g_clientIdEdit = nullptr;
HWND g_appFilterEdit = nullptr;
HWND g_artSourceCombo = nullptr;
HWND g_artCountryCombo = nullptr;
HWND g_statusDisplayCombo = nullptr;
HWND g_pollEdit = nullptr;
HWND g_showPausedCheck = nullptr;
HWND g_rpcEnabledCheck = nullptr;
HWND g_startupCheck = nullptr;
HWND g_statusDiscordLabel = nullptr;
HWND g_statusTrackLabel = nullptr;
HWND g_statusRpcLabel = nullptr;
HWND g_hintLabel = nullptr;

int scale(int value) {
    return MulDiv(value, static_cast<int>(g_dpi), 96);
}

std::wstring trimWide(std::wstring text) {
    const auto isSpace = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
    };
    size_t begin = 0;
    while (begin < text.size() && isSpace(text[begin])) ++begin;
    size_t end = text.size();
    while (end > begin && isSpace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

std::wstring windowText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void setText(HWND hwnd, const std::wstring& text) {
    if (windowText(hwnd) != text) {
        SetWindowTextW(hwnd, text.c_str());
    }
}

void setFont(HWND control, HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

HWND createControl(const wchar_t* className, const wchar_t* text, DWORD style, int id, int x,
                   int y, int width, int height, HFONT font) {
    HWND control = CreateWindowExW(
        0, className, text, WS_CHILD | WS_VISIBLE | style, scale(x), scale(y), scale(width),
        scale(height), g_settingsWindow,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    setFont(control, font);
    return control;
}

HFONT makeFont(int points, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(points, static_cast<int>(g_dpi), 72), 0, 0, 0, weight, FALSE,
                       FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

void showMessage(const std::wstring& text, UINT flags) {
    HWND parent = g_settingsWindow ? g_settingsWindow : g_trayWindow;
    MessageBoxW(parent, text.c_str(), L"Amazon Music Discord RPC", flags);
}

void openPath(const std::wstring& path) {
    if (!path.empty()) {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void openUrl(const wchar_t* url) {
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

bool startupEnabled() {
    wchar_t value[1024]{};
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        L"AmazonMusicRPC", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}

bool setStartupEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        const std::wstring command = L"\"" + executablePath() + L"\"";
        status = RegSetValueExW(key, L"AmazonMusicRPC", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"AmazonMusicRPC");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::wstring tooltipText() {
    const WorkerSnapshot snapshot = g_worker.snapshot();
    std::wstring tip = L"Amazon Music Discord RPC";
    if (!g_config.rpcEnabled) {
        tip += L" — 停止中";
    } else if (snapshot.hasTrack) {
        tip += L" — " + wideFromUtf8(snapshot.title);
        if (!snapshot.artist.empty() && snapshot.artist != snapshot.title) {
            tip += L" / " + wideFromUtf8(snapshot.artist);
        }
    } else if (snapshot.running) {
        tip += L" — 待機中";
    } else {
        tip += L" — 停止中";
    }
    if (tip.size() > 120) tip.resize(120);
    return tip;
}

void updateTrayTooltip() {
    if (!g_trayAdded) return;
    const std::wstring tip = tooltipText();
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void showBalloon(const std::wstring& title, const std::wstring& text) {
    if (!g_trayAdded) return;
    g_nid.uFlags = NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, text.c_str(), _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = 0;
}

HICON loadAppIcon(bool small) {
    const int cx = GetSystemMetrics(small ? SM_CXSMICON : SM_CXICON);
    const int cy = GetSystemMetrics(small ? SM_CYSMICON : SM_CYICON);
    HICON icon = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(kAppIconId),
                                               IMAGE_ICON, cx, cy,
                                               LR_DEFAULTCOLOR | LR_SHARED));
    if (!icon) icon = LoadIconW(nullptr, IDI_APPLICATION);
    return icon;
}

void addTrayIcon() {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_trayWindow;
    g_nid.uID = kTrayIconId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = kTrayMessage;
    g_nid.hIcon = loadAppIcon(true);
    wcsncpy_s(g_nid.szTip, L"Amazon Music Discord RPC", _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayAdded = true;
    }
}

void removeTrayIcon() {
    if (g_trayAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

void updateStatusLabels() {
    if (!g_settingsWindow) return;
    const WorkerSnapshot snapshot = g_worker.snapshot();

    std::wstring discord;
    if (!g_config.rpcEnabled) {
        discord = L"Discord: 停止中";
    } else if (snapshot.discordConnected) {
        discord = L"Discord: 接続済み";
    } else if (!snapshot.discordError.empty()) {
        discord = L"Discord: 未接続 (エラー)";
    } else if (snapshot.running) {
        discord = L"Discord: 未接続 — Discord アプリを起動してください";
    } else {
        discord = L"Discord: 未接続";
    }
    setText(g_statusDiscordLabel, discord);

    std::wstring track;
    if (snapshot.hasTrack) {
        track = snapshot.playing ? L"再生中: " : (snapshot.paused ? L"一時停止: " : L"停止: ");
        track += wideFromUtf8(snapshot.title);
        if (!snapshot.artist.empty() && snapshot.artist != snapshot.title) {
            track += L" — " + wideFromUtf8(snapshot.artist);
        }
        if (snapshot.durationMs > 0) {
            track += L"  (" + wideFromUtf8(formatDuration(snapshot.positionMs)) + L" / " +
                     wideFromUtf8(formatDuration(snapshot.durationMs)) + L")";
        }
    } else {
        track = L"再生中: 待機中 (Amazon Music で再生してください)";
    }
    setText(g_statusTrackLabel, track);

    std::wstring rpc;
    if (!g_config.rpcEnabled) {
        rpc = L"RPC: 停止中";
    } else if (!snapshot.running) {
        rpc = L"RPC: 待機中";
    } else {
        rpc = L"RPC: 有効";
    }
    setText(g_statusRpcLabel, rpc);

    std::wstring hint;
    if (!snapshot.lastError.empty()) {
        hint = wideFromUtf8(snapshot.lastError);
    } else if (!snapshot.discordError.empty()) {
        hint = L"Discord: " + wideFromUtf8(snapshot.discordError);
    } else {
        hint = L"設定は config.ini に保存されます。閉じても通知領域に常駐します。";
    }
    setText(g_hintLabel, hint);

    InvalidateRect(g_statusDiscordLabel, nullptr, TRUE);
    InvalidateRect(g_hintLabel, nullptr, TRUE);
}

void loadConfigIntoControls() {
    if (!g_settingsWindow) return;
    SetWindowTextW(g_clientIdEdit, wideFromUtf8(g_config.clientId).c_str());
    SetWindowTextW(g_appFilterEdit, wideFromUtf8(g_config.appFilter).c_str());

    int artIndex = 0;
    if (g_config.artSource == "catbox") artIndex = 1;
    else if (g_config.artSource == "none") artIndex = 2;
    SendMessageW(g_artSourceCombo, CB_SETCURSEL, artIndex, 0);

    std::wstring country = wideFromUtf8(g_config.artCountry);
    if (country.empty()) country = L"JP";
    const LRESULT countryIndex = SendMessageW(
        g_artCountryCombo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
        reinterpret_cast<LPARAM>(country.c_str()));
    if (countryIndex != CB_ERR) {
        SendMessageW(g_artCountryCombo, CB_SETCURSEL, countryIndex, 0);
    } else {
        SetWindowTextW(g_artCountryCombo, country.c_str());
    }

    int statusIndex = 0;
    if (g_config.statusDisplay == "state") statusIndex = 1;
    else if (g_config.statusDisplay == "name") statusIndex = 2;
    SendMessageW(g_statusDisplayCombo, CB_SETCURSEL, statusIndex, 0);

    SetWindowTextW(g_pollEdit, std::to_wstring(g_config.pollMs).c_str());
    CheckDlgButton(g_settingsWindow, IDC_SHOW_PAUSED,
                   g_config.showPaused ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settingsWindow, IDC_RPC_ENABLED,
                   g_config.rpcEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_settingsWindow, IDC_START_WITH_WINDOWS,
                   startupEnabled() ? BST_CHECKED : BST_UNCHECKED);
    updateStatusLabels();
}

bool configFromControls(Config& next, std::wstring& error) {
    next = g_config;
    next.clientId = trim(utf8FromWide(windowText(g_clientIdEdit)));
    next.appFilter = trim(utf8FromWide(windowText(g_appFilterEdit)));

    const int artIndex = static_cast<int>(SendMessageW(g_artSourceCombo, CB_GETCURSEL, 0, 0));
    next.artSource = artIndex == 1 ? "catbox" : (artIndex == 2 ? "none" : "itunes");

    std::wstring country = trimWide(windowText(g_artCountryCombo));
    if (country.empty()) country = L"JP";
    CharUpperBuffW(country.data(), static_cast<DWORD>(country.size()));
    next.artCountry = utf8FromWide(country);

    const int statusIndex =
        static_cast<int>(SendMessageW(g_statusDisplayCombo, CB_GETCURSEL, 0, 0));
    next.statusDisplay = statusIndex == 1 ? "state" : (statusIndex == 2 ? "name" : "details");

    try {
        next.pollMs = std::stoi(trimWide(windowText(g_pollEdit)));
    } catch (...) {
        error = L"ポーリング間隔は 250〜5000 の数字で入力してください。";
        return false;
    }
    if (next.pollMs < 250 || next.pollMs > 5000) {
        error = L"ポーリング間隔は 250〜5000 の範囲で入力してください。";
        return false;
    }

    next.showPaused =
        IsDlgButtonChecked(g_settingsWindow, IDC_SHOW_PAUSED) == BST_CHECKED;
    next.rpcEnabled =
        IsDlgButtonChecked(g_settingsWindow, IDC_RPC_ENABLED) == BST_CHECKED;

    if (next.clientId.empty()) {
        if (next.rpcEnabled) {
            error = L"Discord Application ID を入力してください。\n\n"
                    L"Discord Developer Portal (https://discord.com/developers/applications) "
                    L"でアプリを作成し、Application ID をコピーしてください。";
            return false;
        }
    } else if (!isValidClientId(next.clientId)) {
        error = L"Application ID は 17〜20 桁の数字で入力してください。";
        return false;
    }
    return true;
}

void saveSettings() {
    Config next;
    std::wstring error;
    if (!configFromControls(next, error)) {
        showMessage(error, MB_OK | MB_ICONWARNING);
        SetFocus(g_clientIdEdit);
        return;
    }

    if (!saveConfig(g_configPath, next)) {
        showMessage(L"config.ini に設定を保存できませんでした。\n"
                    L"書き込み可能な場所に配置してから再試行してください。",
                    MB_OK | MB_ICONERROR);
        return;
    }

    const bool startWithWindows =
        IsDlgButtonChecked(g_settingsWindow, IDC_START_WITH_WINDOWS) == BST_CHECKED;
    if (!setStartupEnabled(startWithWindows)) {
        showMessage(L"Windows の自動起動設定を変更できませんでした。", MB_OK | MB_ICONWARNING);
    }

    g_config = next;
    HCURSOR previousCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    g_worker.start(g_config);
    SetCursor(previousCursor);
    updateTrayTooltip();
    updateStatusLabels();
    setText(g_hintLabel, L"設定を保存しました。Discord への接続を確認しています...");
}

void requestExit() {
    if (g_quitting) return;
    g_quitting = true;
    g_worker.stop();
    removeTrayIcon();
    if (g_settingsWindow) {
        DestroyWindow(g_settingsWindow);
        g_settingsWindow = nullptr;
    }
    if (g_trayWindow) {
        DestroyWindow(g_trayWindow);
        g_trayWindow = nullptr;
    }
    PostQuitMessage(0);
}

void showSettings() {
    if (!g_settingsWindow) return;
    ShowWindow(g_settingsWindow, SW_SHOWNORMAL);
    if (IsIconic(g_settingsWindow)) {
        ShowWindow(g_settingsWindow, SW_RESTORE);
    }
    SetForegroundWindow(g_settingsWindow);
    if (!isValidClientId(g_config.clientId)) {
        SetFocus(g_clientIdEdit);
        SendMessageW(g_clientIdEdit, EM_SETSEL, 0, -1);
    }
}

void toggleRpc() {
    if (!g_config.rpcEnabled && !isValidClientId(g_config.clientId)) {
        showSettings();
        showMessage(L"先に Discord Application ID を設定してください。",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    g_config.rpcEnabled = !g_config.rpcEnabled;
    saveConfig(g_configPath, g_config);
    if (g_settingsWindow) {
        CheckDlgButton(g_settingsWindow, IDC_RPC_ENABLED,
                       g_config.rpcEnabled ? BST_CHECKED : BST_UNCHECKED);
    }
    g_worker.start(g_config);
    updateTrayTooltip();
    updateStatusLabels();
}

void showTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"設定を開く");
    AppendMenuW(menu, MF_STRING, ID_TRAY_TOGGLE,
                g_config.rpcEnabled ? L"RPC を一時停止" : L"RPC を再開");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"終了");

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(g_trayWindow);
    const int command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                       point.x, point.y, 0, g_trayWindow, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_trayWindow, WM_NULL, 0, 0);

    switch (command) {
    case ID_TRAY_SETTINGS: showSettings(); break;
    case ID_TRAY_TOGGLE: toggleRpc(); break;
    case ID_TRAY_EXIT: requestExit(); break;
    default: break;
    }
}

void createControls() {
    g_dpi = GetDpiForWindow(g_settingsWindow);
    if (g_dpi == 0) g_dpi = 96;

    g_font = makeFont(9, FW_NORMAL, L"Yu Gothic UI");
    g_fontBold = makeFont(9, FW_BOLD, L"Yu Gothic UI");
    g_fontTitle = makeFont(15, FW_BOLD, L"Yu Gothic UI");
    g_fontSmall = makeFont(8, FW_NORMAL, L"Yu Gothic UI");

    createControl(L"STATIC", L"Amazon Music Discord RPC", SS_LEFT, 0, 16, 12, 568, 30,
                  g_fontTitle);
    createControl(L"STATIC", L"Amazon Music の再生曲を Discord のプロフィールに表示します。",
                  SS_LEFT, 0, 18, 44, 560, 18, g_fontSmall);

    createControl(L"BUTTON", L"ステータス", BS_GROUPBOX, 0, 16, 68, 568, 94, g_font);
    g_statusDiscordLabel = createControl(L"STATIC", L"Discord: 未接続", SS_LEFT,
                                         IDC_STATUS_DISCORD, 32, 86, 540, 20, g_fontBold);
    g_statusTrackLabel = createControl(L"STATIC", L"再生中: 待機中",
                                       SS_LEFT | SS_ENDELLIPSIS, IDC_STATUS_TRACK, 32, 108, 540,
                                       20, g_font);
    g_statusRpcLabel = createControl(L"STATIC", L"RPC: 停止中", SS_LEFT, IDC_STATUS_RPC, 32,
                                     130, 540, 20, g_font);

    createControl(L"BUTTON", L"設定", BS_GROUPBOX, 0, 16, 168, 568, 402, g_font);

    createControl(L"STATIC", L"Discord Application ID", SS_LEFT, 0, 32, 194, 170, 20, g_font);
    g_clientIdEdit = createControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, IDC_CLIENT_ID, 210,
                                   190, 250, 24, g_font);
    SendMessageW(g_clientIdEdit, EM_SETLIMITTEXT, 20, 0);
    createControl(L"BUTTON", L"取得方法", BS_PUSHBUTTON, IDC_OPEN_PORTAL, 470, 190, 90, 24,
                  g_font);

    createControl(L"STATIC", L"対象アプリ", SS_LEFT, 0, 32, 224, 170, 20, g_font);
    g_appFilterEdit = createControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, IDC_APP_FILTER,
                                    210, 220, 350, 24, g_font);
    SendMessageW(g_appFilterEdit, EM_SETLIMITTEXT, 512, 0);
    createControl(L"STATIC", L"セミコロン区切り / 空欄で全てのアプリ", SS_LEFT, 0, 210, 247,
                  350, 16, g_fontSmall);

    createControl(L"STATIC", L"ジャケット画像", SS_LEFT, 0, 32, 278, 170, 20, g_font);
    g_artSourceCombo = createControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                     IDC_ART_SOURCE, 210, 274, 350, 200, g_font);
    SendMessageW(g_artSourceCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"iTunes の画像 (おすすめ)"));
    SendMessageW(g_artSourceCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Amazon Music の画像 (catbox / 公開アップロード)"));
    SendMessageW(g_artSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"画像なし"));

    createControl(L"STATIC", L"iTunes ストア", SS_LEFT, 0, 32, 312, 170, 20, g_font);
    g_artCountryCombo = createControl(L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL,
                                      IDC_ART_COUNTRY, 210, 308, 120, 200, g_font);
    SendMessageW(g_artCountryCombo, CB_LIMITTEXT, 8, 0);
    for (const wchar_t* country : {L"JP", L"US", L"GB", L"DE", L"FR", L"IT", L"ES", L"CA",
                                   L"AU", L"BR"}) {
        SendMessageW(g_artCountryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(country));
    }

    createControl(L"STATIC", L"ステータス表示", SS_LEFT, 0, 32, 346, 170, 20, g_font);
    g_statusDisplayCombo = createControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                                         IDC_STATUS_DISPLAY, 210, 342, 350, 200, g_font);
    SendMessageW(g_statusDisplayCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"曲名 (おすすめ)"));
    SendMessageW(g_statusDisplayCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"アーティスト"));
    SendMessageW(g_statusDisplayCombo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Discord アプリ名"));

    createControl(L"STATIC", L"ポーリング間隔", SS_LEFT, 0, 32, 380, 170, 20, g_font);
    g_pollEdit = createControl(L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, IDC_POLL_MS,
                               210, 376, 80, 24, g_font);
    SendMessageW(g_pollEdit, EM_SETLIMITTEXT, 5, 0);
    createControl(L"STATIC", L"ms (250〜5000)", SS_LEFT, 0, 300, 380, 250, 20, g_fontSmall);

    g_showPausedCheck = createControl(L"BUTTON", L"一時停止中も表示する", BS_AUTOCHECKBOX,
                                      IDC_SHOW_PAUSED, 210, 410, 300, 22, g_font);
    g_rpcEnabledCheck = createControl(L"BUTTON", L"Discord Rich Presence を有効にする",
                                      BS_AUTOCHECKBOX, IDC_RPC_ENABLED, 210, 436, 300, 22,
                                      g_font);
    g_startupCheck = createControl(L"BUTTON", L"Windows 起動時に自動起動する", BS_AUTOCHECKBOX,
                                   IDC_START_WITH_WINDOWS, 210, 462, 300, 22, g_font);

    createControl(L"BUTTON", L"ログを開く", BS_PUSHBUTTON, IDC_OPEN_LOG, 32, 500, 110, 28,
                  g_font);
    createControl(L"BUTTON", L"設定フォルダを開く", BS_PUSHBUTTON, IDC_OPEN_FOLDER, 150, 500,
                  150, 28, g_font);

    createControl(L"BUTTON", L"保存して適用", BS_DEFPUSHBUTTON, IDC_SAVE, 260, 584, 140, 32,
                  g_fontBold);
    createControl(L"BUTTON", L"キャンセル", BS_PUSHBUTTON, IDC_CANCEL, 410, 584, 100, 32,
                  g_font);
    createControl(L"BUTTON", L"終了", BS_PUSHBUTTON, IDC_EXIT, 520, 584, 64, 32, g_font);

    g_hintLabel = createControl(L"STATIC",
                                L"設定は config.ini に保存されます。閉じても通知領域に常駐します。",
                                SS_LEFT, IDC_HINT, 18, 624, 564, 18, g_fontSmall);

    loadConfigIntoControls();
}

void createSettingsWindow() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
            switch (message) {
            case WM_CREATE:
                g_dpi = GetDpiForWindow(hwnd);
                if (g_dpi == 0) g_dpi = 96;
                return 0;
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wParam);
                HWND control = reinterpret_cast<HWND>(lParam);
                SetBkMode(dc, TRANSPARENT);
                const WorkerSnapshot snapshot = g_worker.snapshot();
                if (control == g_statusDiscordLabel) {
                    COLORREF color = RGB(100, 100, 100);
                    if (snapshot.discordConnected) {
                        color = RGB(0, 128, 0);
                    } else if (!snapshot.discordError.empty() || !snapshot.lastError.empty()) {
                        color = RGB(180, 0, 0);
                    }
                    SetTextColor(dc, color);
                } else if (control == g_hintLabel) {
                    if (!snapshot.lastError.empty() || !snapshot.discordError.empty()) {
                        SetTextColor(dc, RGB(180, 0, 0));
                    } else {
                        SetTextColor(dc, RGB(100, 100, 100));
                    }
                }
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }
            case WM_COMMAND: {
                const int id = LOWORD(wParam);
                const int notification = HIWORD(wParam);
                if ((id == IDC_SAVE || id == IDOK) && notification == BN_CLICKED) {
                    saveSettings();
                } else if (id == IDC_CANCEL && notification == BN_CLICKED) {
                    loadConfigIntoControls();
                    ShowWindow(hwnd, SW_HIDE);
                } else if (id == IDC_EXIT && notification == BN_CLICKED) {
                    requestExit();
                } else if (id == IDC_OPEN_PORTAL && notification == BN_CLICKED) {
                    openUrl(L"https://discord.com/developers/applications");
                } else if (id == IDC_OPEN_LOG && notification == BN_CLICKED) {
                    openPath(g_logPath);
                } else if (id == IDC_OPEN_FOLDER && notification == BN_CLICKED) {
                    openPath(executableDirectory());
                } else if (id == IDCANCEL) {
                    loadConfigIntoControls();
                    ShowWindow(hwnd, SW_HIDE);
                }
                return 0;
            }
            case WM_CLOSE:
                loadConfigIntoControls();
                ShowWindow(hwnd, SW_HIDE);
                if (!g_balloonShown) {
                    showBalloon(L"Amazon Music Discord RPC",
                                L"通知領域に常駐しています。アイコンをクリックすると設定を開きます。");
                    g_balloonShown = true;
                }
                return 0;
            case WM_DESTROY:
                if (g_font) {
                    DeleteObject(g_font);
                    g_font = nullptr;
                }
                if (g_fontBold) {
                    DeleteObject(g_fontBold);
                    g_fontBold = nullptr;
                }
                if (g_fontTitle) {
                    DeleteObject(g_fontTitle);
                    g_fontTitle = nullptr;
                }
                if (g_fontSmall) {
                    DeleteObject(g_fontSmall);
                    g_fontSmall = nullptr;
                }
                g_settingsWindow = nullptr;
                return 0;
            default:
                break;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        };
        wc.hInstance = g_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kSettingsClassName;
        wc.hIcon = loadAppIcon(false);
        wc.hIconSm = loadAppIcon(true);
        if (!RegisterClassExW(&wc)) {
            showMessage(L"設定ウィンドウを登録できませんでした。", MB_OK | MB_ICONERROR);
            return;
        }
        registered = true;
    }

    g_dpi = GetDpiForSystem();
    if (g_dpi == 0) g_dpi = 96;
    RECT rect{0, 0, scale(600), scale(654)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, g_dpi);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - width) / 2);
    const int y = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - height) / 2);

    g_settingsWindow = CreateWindowExW(
        WS_EX_CONTROLPARENT, kSettingsClassName, L"Amazon Music Discord RPC 設定", style, x, y,
        width, height, nullptr, nullptr, g_instance, nullptr);
    if (!g_settingsWindow) {
        showMessage(L"設定ウィンドウを作成できませんでした。", MB_OK | MB_ICONERROR);
        return;
    }
    SendMessageW(g_settingsWindow, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(loadAppIcon(false)));
    SendMessageW(g_settingsWindow, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(loadAppIcon(true)));
    createControls();
}

void createTrayWindow() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = [](HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage) {
                g_trayAdded = false;
                addTrayIcon();
                updateTrayTooltip();
                return 0;
            }
            switch (message) {
            case kTrayMessage:
                switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    showSettings();
                    return 0;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    showTrayMenu();
                    return 0;
                default:
                    return 0;
                }
            case kShowSettingsMessage:
                showSettings();
                return 0;
            case WM_TIMER:
                if (wParam == kTrayTimer) {
                    updateTrayTooltip();
                    if (g_settingsWindow && IsWindowVisible(g_settingsWindow)) {
                        updateStatusLabels();
                    }
                    if (!g_trayAdded && !g_quitting) {
                        addTrayIcon();
                    }
                    return 0;
                }
                break;
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (wParam) {
                    g_worker.stop();
                    removeTrayIcon();
                }
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd, kTrayTimer);
                g_trayWindow = nullptr;
                g_worker.stop();
                removeTrayIcon();
                if (!g_quitting) {
                    g_quitting = true;
                    if (g_settingsWindow) {
                        DestroyWindow(g_settingsWindow);
                        g_settingsWindow = nullptr;
                    }
                }
                PostQuitMessage(0);
                return 0;
            default:
                break;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        };
        wc.hInstance = g_instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kTrayClassName;
        wc.hIcon = loadAppIcon(false);
        wc.hIconSm = loadAppIcon(true);
        if (!RegisterClassExW(&wc)) {
            showMessage(L"通知領域のウィンドウを登録できませんでした。", MB_OK | MB_ICONERROR);
            return;
        }
        registered = true;
    }

    g_trayWindow = CreateWindowExW(0, kTrayClassName, L"Amazon Music Discord RPC", 0, 0, 0, 0,
                                   0, nullptr, nullptr, g_instance, nullptr);
    if (!g_trayWindow) {
        showMessage(L"通知領域のウィンドウを作成できませんでした。", MB_OK | MB_ICONERROR);
        return;
    }
    addTrayIcon();
    SetTimer(g_trayWindow, kTrayTimer, 500, nullptr);
}

struct ConsoleSession {
    bool allocated = false;
    bool available = false;
};

bool openConsole(ConsoleSession& session, bool wait) {
    session = ConsoleSession{};
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        session.available = true;
    } else if (AllocConsole()) {
        session.allocated = true;
        session.available = true;
    }
    if (!session.available) return false;

    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"Amazon Music Discord RPC - 診断");
    std::ios::sync_with_stdio(false);
    if (wait) {
        std::cout << "\n";
    }
    return true;
}

void closeConsole(const ConsoleSession& session, bool wait) {
    if (session.available && wait) {
        std::cout << "\nEnter キーで終了します..." << std::flush;
        std::cin.get();
    }
    if (session.available) {
        FreeConsole();
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR commandLine, int showCommand) {
    (void)previous;
    (void)commandLine;
    (void)showCommand;

    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)) {
        SetProcessDPIAware();
    }
    g_instance = instance;

    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&commonControls);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    bool help = false;
    bool probe = false;
    bool doLookup = false;
    bool noArt = false;
    bool anyApp = false;
    bool verbose = false;
    std::wstring configPath;
    std::string clientIdOverride;
    std::string lookupTitle;
    std::string lookupArtist;
    std::wstring unknown;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h") {
            help = true;
        } else if (arg == L"--probe") {
            probe = true;
        } else if (arg == L"--no-art") {
            noArt = true;
        } else if (arg == L"--any") {
            anyApp = true;
        } else if (arg == L"--verbose" || arg == L"-v") {
            verbose = true;
        } else if (arg == L"--lookup" && i + 1 < argc) {
            doLookup = true;
            lookupTitle = utf8FromWide(argv[++i]);
            if (i + 1 < argc) {
                const std::wstring next = argv[i + 1];
                if (next.rfind(L"--", 0) != 0) {
                    lookupArtist = utf8FromWide(argv[++i]);
                }
            }
        } else if (arg == L"--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == L"--client-id" && i + 1 < argc) {
            clientIdOverride = utf8FromWide(argv[++i]);
        } else if (arg == L"--minimized" || arg == L"--autostart") {
            // Accepted for compatibility with startup shortcuts.
        } else {
            unknown = arg;
            break;
        }
    }

    LocalFree(argv);

    if (!unknown.empty()) {
        ConsoleSession console;
        if (openConsole(console, false)) {
            std::cout << "不明な引数です: " << utf8FromWide(unknown) << "\n\n";
            printUsage();
        } else {
            showMessage(L"不明な引数です: " + unknown, MB_OK | MB_ICONERROR);
        }
        closeConsole(console, console.allocated);
        return 1;
    }

    if (help || probe || doLookup) {
        ConsoleSession console;
        if (!openConsole(console, false)) {
            showMessage(L"コンソールを開けませんでした。", MB_OK | MB_ICONERROR);
            return 1;
        }
        setVerbose(verbose);
        if (help) {
            printUsage();
            closeConsole(console, console.allocated);
            return 0;
        }

        if (configPath.empty()) configPath = executableDirectory() + L"\\config.ini";
        bool created = false;
        Config config = loadConfig(configPath, created);
        if (noArt) config.artSource = "none";
        if (anyApp) config.appFilter.clear();
        if (!clientIdOverride.empty()) config.clientId = clientIdOverride;

        std::cout << "設定ファイル: " << utf8FromWide(configPath) << "\n";
        int result = 0;
        if (doLookup) {
            result = runLookup(config, lookupTitle, lookupArtist);
        } else {
            std::cout << "Amazon Music Discord RPC - メディアセッション診断\n";
            result = runProbe(config);
        }
        closeConsole(console, console.allocated);
        return result;
    }

    if (configPath.empty()) configPath = executableDirectory() + L"\\config.ini";
    g_configPath = configPath;
    g_logPath = executableDirectory() + L"\\AmazonMusicRPC.log";
    setVerbose(verbose);
    setLogFile(g_logPath);
    logInfo("GUI を起動しました。");

    bool created = false;
    g_config = loadConfig(g_configPath, created);
    if (noArt) g_config.artSource = "none";
    if (anyApp) g_config.appFilter.clear();
    if (!clientIdOverride.empty()) g_config.clientId = clientIdOverride;

    g_instanceMutex =
        CreateMutexW(nullptr, FALSE, L"Local\\AmazonMusicRpcSingleInstance");
    if (g_instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            if (HWND existing = FindWindowW(kTrayClassName, nullptr)) {
                PostMessageW(existing, kShowSettingsMessage, 0, 0);
                break;
            }
            Sleep(50);
        }
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return 0;
    }

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    createTrayWindow();
    createSettingsWindow();
    if (!g_trayWindow || !g_settingsWindow) {
        if (g_settingsWindow) DestroyWindow(g_settingsWindow);
        if (g_trayWindow) DestroyWindow(g_trayWindow);
        return 1;
    }

    g_worker.start(g_config);
    if (created || !isValidClientId(g_config.clientId)) {
        showSettings();
        if (g_config.clientId.empty()) {
            setText(g_hintLabel, L"最初に Discord Application ID を設定してください。");
        }
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (g_settingsWindow && IsWindowVisible(g_settingsWindow) &&
            IsDialogMessageW(g_settingsWindow, &message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_worker.stop();
    removeTrayIcon();
    if (g_instanceMutex) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }
    return static_cast<int>(message.wParam);
}
