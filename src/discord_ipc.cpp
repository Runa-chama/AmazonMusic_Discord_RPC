#include "discord_ipc.hpp"

#include "json.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t kOpHandshake = 0;
constexpr uint32_t kOpFrame = 1;
constexpr uint32_t kOpClose = 2;

} // namespace

DiscordIpc::DiscordIpc(std::string clientId) : clientId_(std::move(clientId)) {}

DiscordIpc::~DiscordIpc() {
    disconnect();
}

bool DiscordIpc::tryOpenPipe() {
    for (int index = 0; index < 10; ++index) {
        const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(index);
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            pipe_ = pipe;
            return true;
        }
    }
    return false;
}

bool DiscordIpc::connect() {
    disconnect();
    setLastError({});

    if (!tryOpenPipe()) {
        return false;
    }

    connected_ = false;
    buffered_.clear();

    const std::string handshake =
        "{\"v\":1,\"client_id\":\"" + json::escape(clientId_) + "\"}";
    if (!writeFrame(kOpHandshake, handshake)) {
        disconnect();
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pollIncoming();
        if (connected_) return true;
        if (pipe_ == INVALID_HANDLE_VALUE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    disconnect();
    return false;
}

void DiscordIpc::disconnect() {
    closePipe();
    buffered_.clear();
}

bool DiscordIpc::isConnected() const {
    return connected_ && pipe_ != INVALID_HANDLE_VALUE;
}

std::string DiscordIpc::lastError() const {
    return lastError_;
}

void DiscordIpc::setLastError(const std::string& message) {
    lastError_ = message;
}

void DiscordIpc::poll() {
    pollIncoming();
}

bool DiscordIpc::writeFrame(uint32_t opcode, const std::string& payload) {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    // The 8-byte header and the JSON body are sent as one message. This works
    // for both byte-mode and message-mode named pipes.
    std::string frame;
    frame.resize(8 + payload.size());
    const uint32_t header[2] = {opcode, static_cast<uint32_t>(payload.size())};
    std::memcpy(frame.data(), header, sizeof(header));
    if (!payload.empty()) {
        std::memcpy(frame.data() + 8, payload.data(), payload.size());
    }

    DWORD written = 0;
    if (!WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) ||
        written != frame.size()) {
        logDebug("Discord への書き込みに失敗しました: " + std::to_string(GetLastError()));
        return false;
    }
    return true;
}

std::string DiscordIpc::nextNonce() {
    return "amrpc-" + std::to_string(++nonceCounter_);
}

bool DiscordIpc::setActivity(const std::string& activityJson) {
    if (!isConnected()) return false;

    // Pick up a pending CLOSE / ERROR before writing.
    pollIncoming();
    if (!isConnected()) return false;

    const std::string payload =
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" +
        std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) +
        ",\"activity\":" + activityJson + "},\"nonce\":\"" + nextNonce() + "\"}";

    if (!writeFrame(kOpFrame, payload)) {
        closePipe();
        return false;
    }
    return true;
}

bool DiscordIpc::clearActivity() {
    return setActivity("null");
}

bool DiscordIpc::pollIncoming() {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                error == ERROR_INVALID_HANDLE) {
                closePipe();
            } else {
                logDebug("Discord の状態取得に失敗しました: " + std::to_string(error));
            }
            return false;
        }
        if (available == 0) break;

        char chunk[64 * 1024];
        const DWORD toRead = std::min<DWORD>(available, sizeof(chunk));
        DWORD read = 0;
        const BOOL ok = ReadFile(pipe_, chunk, toRead, &read, nullptr);
        if (!ok && GetLastError() != ERROR_MORE_DATA) {
            logDebug("Discord からの読み取りに失敗しました: " +
                     std::to_string(GetLastError()));
            closePipe();
            return false;
        }
        if (read == 0) break;

        buffered_.append(chunk, read);

        // Parse as many complete frames as the buffer currently holds.
        while (buffered_.size() >= 8) {
            uint32_t header[2]{};
            std::memcpy(header, buffered_.data(), sizeof(header));
            const uint32_t opcode = header[0];
            const uint32_t length = header[1];
            if (length > 16 * 1024 * 1024) {
                logDebug("Discord のフレーム長が不正です。");
                closePipe();
                return false;
            }
            if (buffered_.size() < 8 + static_cast<size_t>(length)) break;

            const std::string payload = buffered_.substr(8, length);
            buffered_.erase(0, 8 + static_cast<size_t>(length));
            handleFrame(opcode, payload);
            if (pipe_ == INVALID_HANDLE_VALUE) return false;
        }
    }

    return isConnected();
}

void DiscordIpc::handleFrame(uint32_t opcode, const std::string& payload) {
    if (opcode == kOpFrame) {
        const auto parsed = json::parse(payload);
        if (parsed && parsed->isObject()) {
            const auto* event = parsed->find("evt");
            if (event && event->isString() && event->asString() == "ERROR") {
                const auto* data = parsed->find("data");
                const auto* message = data ? data->find("message") : nullptr;
                const std::string text =
                    message && message->isString() ? message->asString()
                                                   : std::string("不明なエラー");
                setLastError(text);
                logError("Discord RPC: " + text);
                closePipe();
                return;
            }
        }

        if (!connected_) {
            connected_ = true;
            logInfo("Discord に接続しました。");
        }
        logDebug("Discord <- " + payload);
    } else if (opcode == kOpClose) {
        const auto parsed = json::parse(payload);
        std::string message = "接続が拒否されました。";
        if (parsed && parsed->isObject()) {
            const auto* text = parsed->find("message");
            if (text && text->isString()) message = text->asString();
        }
        setLastError(message);
        logError("Discord RPC: " + message);
        closePipe();
    }
}

void DiscordIpc::closePipe() {
    connected_ = false;
    buffered_.clear();
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
}
