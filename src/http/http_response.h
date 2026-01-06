#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace lb::http {

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
    static HttpResponse service_unavailable(const std::string& message = "Service Unavailable");

private:
    int status_code_;
    std::string reason_phrase_;
    std::unordered_map<std::string, std::string> headers_;
    std::vector<uint8_t> body_;

    [[nodiscard]] std::string get_status_reason() const;
};

} // namespace lb::http
