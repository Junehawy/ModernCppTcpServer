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

    SimpleHttpRequest() = default;
};

SimpleHttpRequest parse_simple_http(const std::string& raw_request);