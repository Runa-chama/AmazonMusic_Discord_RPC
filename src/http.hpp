#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct HttpResponse {
    uint32_t statusCode = 0;
    std::string body;
};

std::optional<HttpResponse> httpGet(const std::string& url,
                                    const std::string& accept = "application/json");

std::optional<HttpResponse> httpPostMultipart(const std::string& url,
                                              const std::string& fieldName,
                                              const std::string& fieldValue,
                                              const std::string& fileName,
                                              const std::string& contentType,
                                              const std::vector<uint8_t>& fileData);

std::string urlEncode(const std::string& text);
