#include "http_types.h"
#include <sstream>
#include <algorithm>

SimpleHttpRequest parse_simple_http(const std::string& raw_request) {
    SimpleHttpRequest req;
    std::istringstream iss(raw_request);
    std::string line;

    if (std::getline(iss, line)) {
        std::istringstream first_line(line);
        first_line >> req.method >> req.path >> req.version;
        if (!req.version.empty() && req.version.back() == '\r') {
            req.version.pop_back();
        }
    }

    // headers
    while (std::getline(iss, line)) {
        if (line.empty() || line == "\r") break;

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);

        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r") + 1);

        std::string key_lower = key;
        std::transform(key_lower.begin(), key_lower.end(), key_lower.begin(), ::tolower);

        req.headers[key] = value;

        if (key_lower == "connection") {
            req.keep_alive = (value != "close");
        } else if (key_lower == "content-length") {
            try { req.content_length = std::stoul(value); } catch (...) {}
        } else if (key_lower == "host") {
            req.host = value;
        } else if (key_lower == "user-agent") {
            req.user_agent = value;
        } else if (key_lower == "content-type") {
            req.content_type = value;
        }
    }

    return req;
}