#include "http/http_response.h"
#include <sstream>

namespace lb::http {

HttpResponse::HttpResponse() : status_code_(200), reason_phrase_("OK") {
    set_header("Content-Type", "text/plain");
    set_header("Connection", "close");
}

void HttpResponse::set_status(HttpStatusCode code) {
    status_code_ = static_cast<int>(code);
    reason_phrase_ = get_status_reason();
}

void HttpResponse::set_status(int code, const std::string& reason) {
    status_code_ = code;
    reason_phrase_ = reason;
}

void HttpResponse::set_header(const std::string& name, const std::string& value) {
    headers_[name] = value;
}

void HttpResponse::set_body(const std::vector<uint8_t>& body) {
    body_ = body;
    set_header("Content-Length", std::to_string(body_.size()));
}

void HttpResponse::set_body(const std::string& body) {
    body_.assign(body.begin(), body.end());
    set_header("Content-Length", std::to_string(body_.size()));
}

std::string HttpResponse::to_string() const {
    std::ostringstream oss;

    oss << "HTTP/1.1 " << status_code_ << " " << reason_phrase_ << "\r\n";

    for (const auto& [name, value] : headers_) {
        oss << name << ": " << value << "\r\n";
    }

    oss << "\r\n";

    if (!body_.empty()) {
        oss.write(reinterpret_cast<const char*>(body_.data()), body_.size());
    }

    return oss.str();
}

std::vector<uint8_t> HttpResponse::to_bytes() const {
    std::string str = to_string();
    return std::vector<uint8_t>(str.begin(), str.end());
}

std::string HttpResponse::get_status_reason() const {
    switch (status_code_) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Unknown";
    }
}

HttpResponse HttpResponse::error_response(HttpStatusCode code, const std::string& message) {
    HttpResponse resp;
    resp.set_status(code);
    resp.set_header("Content-Type", "text/plain");
    resp.set_body(message);
    return resp;
}

HttpResponse HttpResponse::bad_request(const std::string& message) {
    return error_response(HttpStatusCode::BAD_REQUEST, message);
}

HttpResponse HttpResponse::not_found(const std::string& message) {
    return error_response(HttpStatusCode::NOT_FOUND, message);
}

HttpResponse HttpResponse::service_unavailable(const std::string& message) {
    return error_response(HttpStatusCode::SERVICE_UNAVAILABLE, message);
}

} // namespace lb::http
