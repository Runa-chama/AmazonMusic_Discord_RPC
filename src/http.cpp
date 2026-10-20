#include "http.hpp"

#include "util.hpp"

#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <random>

namespace {

struct InternetHandle {
    HINTERNET handle = nullptr;
    ~InternetHandle() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

std::optional<HttpResponse> request(const std::string& url, const std::wstring& method,
                                    const std::string& body,
                                    const std::wstring& contentType,
                                    const std::string& accept) {
    const std::wstring wideUrl = wideFromUtf8(url);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), static_cast<DWORD>(wideUrl.size()), 0, &components)) {
        logDebug("URL を解析できませんでした: " + url);
        return std::nullopt;
    }

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }

    InternetHandle session;
    session.handle = WinHttpOpen(L"AmazonMusicRPC/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.handle) {
        session.handle = WinHttpOpen(L"AmazonMusicRPC/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!session.handle) return std::nullopt;

    WinHttpSetTimeouts(session.handle, 5000, 5000, 8000, 8000);

    InternetHandle connection;
    connection.handle =
        WinHttpConnect(session.handle, host.c_str(), components.nPort, 0);
    if (!connection.handle) return std::nullopt;

    const DWORD flags =
        components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle req;
    req.handle = WinHttpOpenRequest(connection.handle, method.c_str(), path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req.handle) return std::nullopt;

    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(req.handle, WINHTTP_OPTION_DECOMPRESSION, &decompression,
                     sizeof(decompression));

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy,
                     sizeof(redirectPolicy));

    std::wstring headers;
    if (!accept.empty()) headers += L"Accept: " + wideFromUtf8(accept) + L"\r\n";
    if (!contentType.empty()) headers += L"Content-Type: " + contentType + L"\r\n";
    headers += L"Cache-Control: no-cache\r\n";

    const bool isPost = !body.empty();
    LPVOID bodyData = isPost ? const_cast<char*>(body.data()) : WINHTTP_NO_REQUEST_DATA;
    const DWORD bodySize = isPost ? static_cast<DWORD>(body.size()) : 0;

    if (!WinHttpSendRequest(req.handle, headers.c_str(), static_cast<DWORD>(-1L), bodyData,
                            bodySize, bodySize, 0)) {
        logDebug("HTTP 送信に失敗しました: " + std::to_string(GetLastError()));
        return std::nullopt;
    }

    if (!WinHttpReceiveResponse(req.handle, nullptr)) {
        logDebug("HTTP 応答の受信に失敗しました: " + std::to_string(GetLastError()));
        return std::nullopt;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(req.handle,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }

    HttpResponse response;
    response.statusCode = statusCode;

    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req.handle, &available)) break;
        if (available == 0) break;

        const size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(req.handle, response.body.data() + offset, available, &read)) {
            response.body.resize(offset);
            break;
        }
        response.body.resize(offset + read);
        if (read == 0) break;
        if (response.body.size() > 8 * 1024 * 1024) break;
    }

    return response;
}

} // namespace

std::optional<HttpResponse> httpGet(const std::string& url, const std::string& accept) {
    return request(url, L"GET", {}, {}, accept);
}

std::optional<HttpResponse> httpPostMultipart(const std::string& url,
                                              const std::string& fieldName,
                                              const std::string& fieldValue,
                                              const std::string& fileName,
                                              const std::string& contentType,
                                              const std::vector<uint8_t>& fileData) {
    std::random_device random;
    char boundaryBuffer[64];
    std::snprintf(boundaryBuffer, sizeof(boundaryBuffer),
                  "----AmazonMusicRPC%08x%08x", random(), random());
    const std::string boundary = boundaryBuffer;

    std::string body;
    body.reserve(fileData.size() + 1024);
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"" + fieldName + "\"\r\n\r\n";
    body += fieldValue + "\r\n";
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"" + fileName +
            "\"\r\n";
    body += "Content-Type: " + contentType + "\r\n\r\n";
    body.append(reinterpret_cast<const char*>(fileData.data()), fileData.size());
    body += "\r\n--" + boundary + "--\r\n";

    return request(url, L"POST", body,
                   L"multipart/form-data; boundary=" + wideFromUtf8(boundary), "text/plain");
}

std::string urlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size() * 3);
    for (const unsigned char c : text) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            result.push_back(static_cast<char>(c));
        } else {
            result.push_back('%');
            result.push_back(hex[(c >> 4) & 0xF]);
            result.push_back(hex[c & 0xF]);
        }
    }
    return result;
}
