#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lb::http {

enum class HttpMethod { GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS, UNKNOWN };

enum class HttpVersion { HTTP_1_0, HTTP_1_1, HTTP_2_0, UNKNOWN };

struct HttpRequest {
    HttpRequest();

    HttpMethod method;
    std::string path;
    std::string query_string;
    HttpVersion version;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    [[nodiscard]] std::string get_header(const std::string& name) const;
    [[nodiscard]] bool has_header(const std::string& name) const;
    [[nodiscard]] std::string get_host() const;
    [[nodiscard]] std::string get_client_ip() const;
    [[nodiscard]] std::string get_cookie(const std::string& name) const;
};

class HttpRequestParser {
public:
    HttpRequestParser();

    bool parse(const std::vector<uint8_t>& buffer, size_t& consumed);

    [[nodiscard]] const HttpRequest& request() const {
        return request_;
    }

    void reset();

    [[nodiscard]] bool has_error() const {
        return has_error_;
    }

    [[nodiscard]] std::string error_message() const {
        return error_message_;
    }

private:
    enum class ParseState { REQUEST_LINE, HEADERS, BODY, COMPLETE, ERROR };

    bool parse_request_line(const std::string& line);
    bool parse_header(const std::string& line);
    bool parse_body(const std::vector<uint8_t>& buffer, size_t& consumed);

    HttpRequest request_;
    ParseState state_;
    bool has_error_;
    std::string error_message_;
    size_t content_length_;
    std::string current_line_;
};

HttpMethod method_from_string(const std::string& method);
std::string method_to_string(HttpMethod method);
HttpVersion version_from_string(const std::string& version);
std::string version_to_string(HttpVersion version);

} // namespace lb::http
