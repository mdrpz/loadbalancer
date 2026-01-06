#include "http/http_handler.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <algorithm>
#include <cstring>

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
