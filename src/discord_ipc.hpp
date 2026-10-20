#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// Minimal Discord IPC client.
//
// The Discord named pipe is opened for synchronous I/O. A blocking ReadFile
// issued while a write is pending can stall WriteFile on the same handle, so
// this implementation never blocks on reads: incoming data is collected with
// PeekNamedPipe from poll(), connect() and setActivity().
class DiscordIpc {
public:
    explicit DiscordIpc(std::string clientId);

    DiscordIpc(const DiscordIpc&) = delete;
    DiscordIpc& operator=(const DiscordIpc&) = delete;

    ~DiscordIpc();

    bool connect();
    void disconnect();
    bool isConnected() const;
    std::string lastError() const;

    // Process any data Discord has already sent us. Safe to call frequently.
    void poll();

    bool setActivity(const std::string& activityJson);
    bool clearActivity();

private:
    bool tryOpenPipe();
    bool writeFrame(uint32_t opcode, const std::string& payload);
    bool pollIncoming();
    void handleFrame(uint32_t opcode, const std::string& payload);
    void closePipe();
    void setLastError(const std::string& message);
    std::string nextNonce();

    std::string clientId_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::string buffered_;
    std::string lastError_;
    bool connected_ = false;
    uint64_t nonceCounter_ = 0;
};
