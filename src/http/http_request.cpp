#include "http/http_request.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace lb::http {

HttpRequest::HttpRequest() : method(HttpMethod::UNKNOWN), version(HttpVersion::UNKNOWN) {}

std::string HttpRequest::get_header(const std::string& name) const {
    auto it = headers.find(name);
    if (it != headers.end())
        return it->second;

    for (const auto& [key, value] : headers) {
        if (key.length() == name.length()) {
            bool match = true;
            for (size_t i = 0; i < key.length(); ++i) {
                if (std::tolower(key[i]) != std::tolower(name[i])) {
                    match = false;
                    break;
                }
            }
            if (match)
                return value;
        }
    }
    return "";
}

bool HttpRequest::has_header(const std::string& name) const {
    return !get_header(name).empty();
}

std::string HttpRequest::get_host() const {
    return get_header("Host");
}

std::string HttpRequest::get_client_ip() const {
    std::string xff = get_header("X-Forwarded-For");
    if (!xff.empty()) {
        size_t comma = xff.find(',');
        if (comma != std::string::npos)
            return xff.substr(0, comma);
        return xff;
    }

    std::string xri = get_header("X-Real-IP");
    if (!xri.empty())
        return xri;

    return "";
}

std::string HttpRequest::get_cookie(const std::string& name) const {
    std::string cookie_header = get_header("Cookie");
    if (cookie_header.empty()) {
        return "";
    }

    size_t pos = 0;
    while (pos < cookie_header.length()) {
        while (pos < cookie_header.length() &&
               (cookie_header[pos] == ' ' || cookie_header[pos] == ';')) {
            pos++;
        }
        if (pos >= cookie_header.length())
            break;

        size_t name_start = pos;
        size_t equals_pos = cookie_header.find('=', pos);
        if (equals_pos == std::string::npos)
            break;

        std::string cookie_name = cookie_header.substr(name_start, equals_pos - name_start);
        while (!cookie_name.empty() && cookie_name[0] == ' ')
            cookie_name.erase(0, 1);
        while (!cookie_name.empty() && cookie_name.back() == ' ')
            cookie_name.pop_back();

        bool match = true;
        if (cookie_name.length() == name.length()) {
            for (size_t i = 0; i < cookie_name.length(); ++i) {
                if (std::tolower(cookie_name[i]) != std::tolower(name[i])) {
                    match = false;
                    break;
                }
            }
        } else {
            match = false;
        }

        if (match) {
            size_t value_start = equals_pos + 1;
            while (value_start < cookie_header.length() && cookie_header[value_start] == ' ')
                value_start++;

            size_t value_end = value_start;
            while (value_end < cookie_header.length() && cookie_header[value_end] != ';')
                value_end++;

            std::string cookie_value = cookie_header.substr(value_start, value_end - value_start);
            while (!cookie_value.empty() && cookie_value.back() == ' ')
                cookie_value.pop_back();

            return cookie_value;
        }

        pos = cookie_header.find(';', equals_pos);
        if (pos == std::string::npos)
            break;
        pos++;
    }

    return "";
}

HttpRequestParser::HttpRequestParser()
    : state_(ParseState::REQUEST_LINE), has_error_(false), content_length_(0) {}

void HttpRequestParser::reset() {
    request_ = HttpRequest();
    state_ = ParseState::REQUEST_LINE;
    has_error_ = false;
    error_message_.clear();
    content_length_ = 0;
    current_line_.clear();
}

bool HttpRequestParser::parse(const std::vector<uint8_t>& buffer, size_t& consumed) {
    consumed = 0;

    if (has_error_)
        return false;

    size_t pos = 0;

    while (state_ == ParseState::REQUEST_LINE || state_ == ParseState::HEADERS) {
        size_t line_end = pos;
        while (line_end < buffer.size() && buffer[line_end] != '\r' && buffer[line_end] != '\n')
            ++line_end;

        if (line_end >= buffer.size())
            return false;

        size_t line_len = line_end - pos;
        if (line_end < buffer.size() && buffer[line_end] == '\r' && line_end + 1 < buffer.size() &&
            buffer[line_end + 1] == '\n') {
            line_end += 2;
        } else if (line_end < buffer.size() && buffer[line_end] == '\n') {
            line_end += 1;
        } else {
            return false;
        }

        std::string line(reinterpret_cast<const char*>(buffer.data() + pos), line_len);
        pos = line_end;
        consumed = pos;

        if (state_ == ParseState::REQUEST_LINE) {
            if (!parse_request_line(line))
                return false;
            state_ = ParseState::HEADERS;
        } else if (state_ == ParseState::HEADERS) {
            if (line.empty()) {
                state_ = ParseState::BODY;
                break;
            }
            if (!parse_header(line))
                return false;
        }
    }

    if (state_ == ParseState::BODY) {
        if (!parse_body(buffer, consumed))
            return false;
        state_ = ParseState::COMPLETE;
    }

    return state_ == ParseState::COMPLETE;
}

bool HttpRequestParser::parse_request_line(const std::string& line) {
    std::istringstream iss(line);
    std::string method_str;
    std::string path_version;

    if (!(iss >> method_str >> path_version)) {
        has_error_ = true;
        error_message_ = "Invalid request line";
        return false;
    }

    request_.method = method_from_string(method_str);

    size_t query_pos = path_version.find('?');
    if (query_pos != std::string::npos) {
        request_.path = path_version.substr(0, query_pos);
        request_.query_string = path_version.substr(query_pos + 1);
    } else {
        request_.path = path_version;
    }

    std::string version_str;
    if (!(iss >> version_str)) {
        has_error_ = true;
        error_message_ = "Missing HTTP version";
        return false;
    }

    request_.version = version_from_string(version_str);

    return true;
}

bool HttpRequestParser::parse_header(const std::string& line) {
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
        has_error_ = true;
        error_message_ = "Invalid header format";
        return false;
    }

    std::string name = line.substr(0, colon_pos);
    std::string value = line.substr(colon_pos + 1);

    while (!value.empty() && (value[0] == ' ' || value[0] == '\t'))
        value.erase(0, 1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.pop_back();

    request_.headers[name] = value;

    if (name == "Content-Length" || name == "content-length") {
        try {
            content_length_ = std::stoul(value);
        } catch (...) {
            has_error_ = true;
            error_message_ = "Invalid Content-Length";
            return false;
        }
    }

    return true;
}

bool HttpRequestParser::parse_body(const std::vector<uint8_t>& buffer, size_t& consumed) {
    size_t body_start = consumed;
    size_t body_available = buffer.size() - body_start;

    if (content_length_ == 0) {
        return true;
    }

    if (body_available < content_length_) {
        return false;
    }

    request_.body.assign(buffer.begin() + body_start,
                         buffer.begin() + body_start + content_length_);
    consumed += content_length_;

    return true;
}

HttpMethod method_from_string(const std::string& method) {
    if (method == "GET")
        return HttpMethod::GET;
    if (method == "POST")
        return HttpMethod::POST;
    if (method == "PUT")
        return HttpMethod::PUT;
    if (method == "DELETE")
        return HttpMethod::DELETE;
    if (method == "PATCH")
        return HttpMethod::PATCH;
    if (method == "HEAD")
        return HttpMethod::HEAD;
    if (method == "OPTIONS")
        return HttpMethod::OPTIONS;
    return HttpMethod::UNKNOWN;
}

std::string method_to_string(HttpMethod method) {
    switch (method) {
    case HttpMethod::GET:
        return "GET";
    case HttpMethod::POST:
        return "POST";
    case HttpMethod::PUT:
        return "PUT";
    case HttpMethod::DELETE:
        return "DELETE";
    case HttpMethod::PATCH:
        return "PATCH";
    case HttpMethod::HEAD:
        return "HEAD";
    case HttpMethod::OPTIONS:
        return "OPTIONS";
    default:
        return "UNKNOWN";
    }
}

HttpVersion version_from_string(const std::string& version) {
    if (version == "HTTP/1.0")
        return HttpVersion::HTTP_1_0;
    if (version == "HTTP/1.1")
        return HttpVersion::HTTP_1_1;
    if (version == "HTTP/2.0" || version == "HTTP/2")
        return HttpVersion::HTTP_2_0;
    return HttpVersion::UNKNOWN;
}

std::string version_to_string(HttpVersion version) {
    switch (version) {
    case HttpVersion::HTTP_1_0:
        return "HTTP/1.0";
    case HttpVersion::HTTP_1_1:
        return "HTTP/1.1";
    case HttpVersion::HTTP_2_0:
        return "HTTP/2.0";
    default:
        return "HTTP/1.1";
    }
}

} // namespace lb::http
