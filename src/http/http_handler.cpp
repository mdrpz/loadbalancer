#include "http/http_handler.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace lb::http {

void HttpHandler::modify_request_headers(HttpRequest& request, const std::string& client_ip,
                                         bool is_https) {
    std::string existing_xff = request.get_header("X-Forwarded-For");
    if (existing_xff.empty()) {
        request.headers["X-Forwarded-For"] = client_ip;
    } else {
        request.headers["X-Forwarded-For"] = existing_xff + ", " + client_ip;
    }

    if (!client_ip.empty()) {
        request.headers["X-Real-IP"] = client_ip;
    }

    request.headers["X-Forwarded-Proto"] = is_https ? "https" : "http";

    std::string host = request.get_host();
    if (!host.empty()) {
        request.headers["X-Forwarded-Host"] = host;
    }
}

void HttpHandler::apply_custom_headers(
    HttpRequest& request, const std::unordered_map<std::string, std::string>& headers_to_add,
    const std::vector<std::string>& headers_to_remove) {
    for (const auto& header_name : headers_to_remove) {
        auto it = request.headers.begin();
        while (it != request.headers.end()) {
            std::string key_lower = it->first;
            std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            std::string target_lower = header_name;
            std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            if (key_lower == target_lower) {
                it = request.headers.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& [name, value] : headers_to_add) {
        if (value.empty()) {
            auto it = request.headers.begin();
            while (it != request.headers.end()) {
                std::string key_lower = it->first;
                std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::string target_lower = name;
                std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                if (key_lower == target_lower) {
                    it = request.headers.erase(it);
                } else {
                    ++it;
                }
            }
        } else {
            bool found = false;
            for (auto& [key, val] : request.headers) {
                std::string key_lower = key;
                std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::string target_lower = name;
                std::transform(target_lower.begin(), target_lower.end(), target_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                if (key_lower == target_lower) {
                    val = value;
                    found = true;
                    break;
                }
            }
            if (!found) {
                request.headers[name] = value;
            }
        }
    }
}

bool HttpHandler::is_http_request(const std::vector<uint8_t>& buffer) {
    if (buffer.size() < 4)
        return false;

    const char* methods[] = {"GET ", "POST ", "PUT ", "DELETE ", "PATCH ", "HEAD ", "OPTIONS "};

    for (const char* method : methods) {
        if (buffer.size() >= strlen(method)) {
            bool match = true;
            for (size_t i = 0; i < strlen(method); ++i) {
                if (buffer[i] != method[i]) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
    }

    return false;
}

std::string HttpHandler::extract_client_ip(int client_fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if (getpeername(client_fd, reinterpret_cast<struct sockaddr*>(&addr), &len) == 0) {
        char ip_str[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &addr.sin_addr, ip_str, INET_ADDRSTRLEN) != nullptr) {
            return std::string(ip_str);
        }
    }

    return "";
}

} // namespace lb::http
