#pragma once
#include <map>
#include <string>

// Simplified HTTP request representation
struct SimpleHttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;

    bool keep_alive = true;
    size_t content_length = 0;

    std::string host;
    std::string user_agent;
    std::string content_type;
    std::string body;

    bool parse_sucess = false;
    std::string parser_error;
};

// Incremental HTTP parser
class HttpParser {
public:
    enum class State { ExpectRequestLine, ExpectHeader, ExpectBody, Complete, Error };

    HttpParser();
    // Feed data incrementally, returns bytes consumed
    size_t parse(const char *data, size_t len);

    bool is_complete() const { return state_ == State::Complete; }
    bool has_error() const { return state_ == State::Error; }

    SimpleHttpRequest get_request();    // Move semantics, resets parser

    void reset();

    const SimpleHttpRequest &partial_request() const { return current_req_; }

private:
    State state_;
    SimpleHttpRequest current_req_;     // Accumulating request
    std::string temp_buffer_;           // Partial line buffer
    size_t body_received_;

    bool parse_request_line(const std::string &line);
    bool parse_header_line(const std::string &line);
    void check_headers();
};

SimpleHttpRequest parse_simple_http(const std::string &raw_request);
