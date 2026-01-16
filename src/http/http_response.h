#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "http/http_request.h"

namespace lb::http {

struct ParsedHttpResponse {
    ParsedHttpResponse();

    int status_code;
    std::string reason_phrase;
    HttpVersion version;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
};

class HttpResponseParser {
public:
    HttpResponseParser();

    bool parse(const std::vector<uint8_t>& buffer, size_t& consumed);

    [[nodiscard]] const ParsedHttpResponse& response() const {
        return response_;
    }

    void reset();

    [[nodiscard]] bool has_error() const {
        return has_error_;
    }

    [[nodiscard]] std::string error_message() const {
        return error_message_;
    }

private:
    enum class ParseState { STATUS_LINE, HEADERS, BODY, COMPLETE, ERROR };

    bool parse_status_line(const std::string& line);
    bool parse_header(const std::string& line);
    bool parse_body(const std::vector<uint8_t>& buffer, size_t& consumed);

    ParsedHttpResponse response_;
    ParseState state_;
    bool has_error_;
    std::string error_message_;
    size_t content_length_;
    std::string current_line_;
};

enum class HttpStatusCode {
    OK = 200,
    CREATED = 201,
    NO_CONTENT = 204,
    BAD_REQUEST = 400,
    UNAUTHORIZED = 401,
    FORBIDDEN = 403,
    NOT_FOUND = 404,
    METHOD_NOT_ALLOWED = 405,
    REQUEST_TIMEOUT = 408,
    TOO_MANY_REQUESTS = 429,
    INTERNAL_SERVER_ERROR = 500,
    BAD_GATEWAY = 502,
    SERVICE_UNAVAILABLE = 503,
    GATEWAY_TIMEOUT = 504
};

class HttpResponse {
public:
    HttpResponse();

    void set_status(HttpStatusCode code);
    void set_status(int code, const std::string& reason);
    void set_header(const std::string& name, const std::string& value);
    void set_body(const std::vector<uint8_t>& body);
    void set_body(const std::string& body);

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::vector<uint8_t> to_bytes() const;

    static HttpResponse error_response(HttpStatusCode code, const std::string& message);
    static HttpResponse bad_request(const std::string& message = "Bad Request");
    static HttpResponse not_found(const std::string& message = "Not Found");
    static HttpResponse forbidden(const std::string& message = "Forbidden");
    static HttpResponse service_unavailable(const std::string& message = "Service Unavailable");
    static HttpResponse too_many_requests(const std::string& message = "Too Many Requests");

private:
    int status_code_;
    std::string reason_phrase_;
    std::unordered_map<std::string, std::string> headers_;
    std::vector<uint8_t> body_;

    [[nodiscard]] std::string get_status_reason() const;
};

} // namespace lb::http
