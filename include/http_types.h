#pragma once
#include <string>
#include <map>

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

class HttpParser {
    public:
    enum class State {
        ExpectRequestLine,
        ExpectHeader,
        ExpectBody,
        Complete,
        Error
    };

    HttpParser();

    size_t parse(const char* data, size_t len);

    bool is_complete() const {return state_ == State::Complete;}
    bool has_error() const {return state_ == State::Error;}

    SimpleHttpRequest get_request();

    void reset();

    const SimpleHttpRequest& partial_request() const {return current_req_;}

private:
    State state_;
    SimpleHttpRequest current_req_;
    std::string temp_buffer_;
    size_t body_received_;

    bool parse_request_line(const std::string& line);
    bool parse_header_line(const std::string& line);
    void check_headers();
};

SimpleHttpRequest parse_simple_http(const std::string& raw_request);