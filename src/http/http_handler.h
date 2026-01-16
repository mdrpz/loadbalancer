#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include "http/http_request.h"
#include "http/http_response.h"

namespace lb::http {

class HttpHandler {
public:
    static void modify_request_headers(HttpRequest& request, const std::string& client_ip,
                                       bool is_https);

    static void apply_custom_headers(
        HttpRequest& request, const std::unordered_map<std::string, std::string>& headers_to_add,
        const std::vector<std::string>& headers_to_remove);

    static void apply_custom_headers(
        ParsedHttpResponse& response,
        const std::unordered_map<std::string, std::string>& headers_to_add,
        const std::vector<std::string>& headers_to_remove);

    static bool is_http_request(const std::vector<uint8_t>& buffer);

    static std::string extract_client_ip(int client_fd);
};

} // namespace lb::http
