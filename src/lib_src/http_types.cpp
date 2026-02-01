#include "http_types.h"
#include <algorithm>
#include <cstring>
#include <sstream>

#include "config.h"
#include "net_utils.h"
#include "stringUtils.h"

HttpParser::HttpParser() : state_(State::ExpectRequestLine), body_received_(0){}

void HttpParser::reset() {
    state_ = State::ExpectRequestLine;
    current_req_ = SimpleHttpRequest();
    temp_buffer_.clear();
    body_received_ = 0;
}

size_t HttpParser::parse(const char *data, size_t len) {
    size_t consumed = 0;

    while (consumed < len && state_ != State::Complete && state_ != State::Error) {
        if (state_ == State::ExpectRequestLine || state_ == State::ExpectHeader) {
            const char *crlf = static_cast<const char *>(memmem(data + consumed, len - consumed, "\r\n", 2));
            if (!crlf) {
                temp_buffer_.append(data + consumed, len - consumed);
                if (temp_buffer_.size() > MAX_HTTP_HEADER_SIZE) {
                    state_ = State::Error;
                    current_req_.parser_error = "Header too large";
                }
                return len;
            }

            size_t line_len = crlf - (data + consumed);
            std::string line = temp_buffer_;
            line.append(data + consumed, line_len);
            temp_buffer_.clear();
            consumed += line_len + 2;

            if (state_ == State::ExpectRequestLine) {
                if (!parse_request_line(line)) {
                    state_ = State::Error;
                    return consumed;
                }
                state_ = State::ExpectHeader;
            } else {
                if (line.empty()) {
                    check_headers();
                    if (current_req_.content_length > 0) {
                        state_ = State::ExpectBody;
                    } else {
                        state_ = State::Complete;
                    }
                } else {
                    if (!parse_header_line(line)) {
                        state_ = State::Error;
                        return consumed;
                    }
                }
            }

        } else if (state_ == State::ExpectBody) {
            size_t need = current_req_.content_length - body_received_;
            size_t available = len - consumed;
            size_t take = std::min(need, available);

            current_req_.body.append(data + consumed, take);
            body_received_ += take;
            consumed += take;

            if (body_received_ >= current_req_.content_length) {
                state_ = State::Complete;
            }
        }
    }
    return consumed;
}

bool HttpParser::parse_request_line(const std::string& line) {
    std::string cleaned_line = line;
    if (!cleaned_line.empty() && cleaned_line.back() == '\r') {
        cleaned_line.pop_back();
    }

    std::istringstream iss(cleaned_line);
    if (!(iss >> current_req_.method >> current_req_.path >> current_req_.version)) {
        current_req_.parser_error = "Invalid request line";
        return false;
    }

    if (current_req_.version.size() >= 5 && current_req_.version.substr(0,5) != "HTTP/") {
        current_req_.parser_error = "Invalid HTTP version";
        return false;
    }
    return true;
}

bool HttpParser::parse_header_line(const std::string &line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) {
        current_req_.parser_error = "Invalid HTTP header line: " + line;
        return false;
    }

    std::string key = trim(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));

    if (key.empty()) {
        current_req_.parser_error = "Empty header key";
        return false;
    }

    current_req_.headers[key] = value;

    std::string key_lower = to_lower(key);
    if (key_lower == "content-length") {
        try {
            current_req_.content_length = std::stoul(value);
            if (current_req_.content_length > MAX_HTTP_BODY_SIZE) {
                current_req_.parser_error = "Content too large";
                return false;
            }
        }catch (...) {
            current_req_.parser_error = "Invalid content-length";
            return false;
        }
    }else if (key_lower == "connection") {
        current_req_.keep_alive = (to_lower(value) != "close");
    }else if (key_lower == "host") {
        current_req_.host = value;
    }else if (key_lower == "user-agent") {
        current_req_.user_agent = value;
    }else if (key_lower == "content-type") {
        current_req_.content_type = value;
    }
    return true;
}

void HttpParser::check_headers() {
    if (current_req_.version == "HTTP/1.0") {
        auto it = current_req_.headers.find("Connection");
        current_req_.keep_alive = (it != current_req_.headers.end() && to_lower(it->second) == "keep-alive");
    }
}

SimpleHttpRequest HttpParser::get_request() {
    SimpleHttpRequest result = std::move(current_req_);
    result.parse_sucess = (state_ == State::Complete);
    reset();
    return result;
}