#include <algorithm>
#include <cctype>
#include <sstream>
#include "http/http_response.h"

namespace lb::http {

ParsedHttpResponse::ParsedHttpResponse()
    : status_code(200), reason_phrase("OK"), version(HttpVersion::HTTP_1_1) {}

HttpResponseParser::HttpResponseParser()
    : state_(ParseState::STATUS_LINE), has_error_(false), content_length_(0) {}

void HttpResponseParser::reset() {
    response_ = ParsedHttpResponse();
    state_ = ParseState::STATUS_LINE;
    has_error_ = false;
    error_message_.clear();
    content_length_ = 0;
    current_line_.clear();
}

bool HttpResponseParser::parse(const std::vector<uint8_t>& buffer, size_t& consumed) {
    consumed = 0;

    if (has_error_)
        return false;

    size_t pos = 0;

    while (state_ == ParseState::STATUS_LINE || state_ == ParseState::HEADERS) {
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

        if (state_ == ParseState::STATUS_LINE) {
            if (!parse_status_line(line))
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

bool HttpResponseParser::parse_status_line(const std::string& line) {
    std::istringstream iss(line);
    std::string version_str;
    std::string status_str;
    std::string reason;

    if (!(iss >> version_str >> status_str)) {
        has_error_ = true;
        error_message_ = "Invalid status line";
        return false;
    }

    response_.version = version_from_string(version_str);

    try {
        response_.status_code = std::stoi(status_str);
    } catch (...) {
        has_error_ = true;
        error_message_ = "Invalid status code";
        return false;
    }

    std::getline(iss, reason);
    while (!reason.empty() && (reason[0] == ' ' || reason[0] == '\t'))
        reason.erase(0, 1);
    response_.reason_phrase = reason;

    return true;
}

bool HttpResponseParser::parse_header(const std::string& line) {
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

    response_.headers[name] = value;

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

bool HttpResponseParser::parse_body(const std::vector<uint8_t>& buffer, size_t& consumed) {
    size_t body_start = consumed;
    size_t body_available = buffer.size() - body_start;

    if (content_length_ > 0) {
        size_t body_to_read = std::min(body_available, content_length_);
        response_.body.assign(buffer.begin() + body_start,
                              buffer.begin() + body_start + body_to_read);
        consumed += body_to_read;
        return response_.body.size() == content_length_;
    }

    response_.body.assign(buffer.begin() + body_start, buffer.end());
    consumed = buffer.size();
    return true;
}

} // namespace lb::http
