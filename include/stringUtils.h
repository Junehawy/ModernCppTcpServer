#pragma once
#include <algorithm>
#include <string>

// Remove whitespace characters from both start and end of a string
inline std::string trim(const std::string &str) {
    const auto start = std::ranges::find_if(str, [](const unsigned char ch) { return !isspace(ch); });
    const auto end = std::find_if(str.rbegin(), str.rend(), [](const unsigned char ch) { return !isspace(ch); }).base();
    return (start < end) ? std::string(start, end) : std::string();
}

//  Remove trailing carriage return character ('\r') from string if existed
inline std::string rtrim_cc(const std::string &str) {
    if (!str.empty() && str.back() == '\r')
        return str.substr(0, str.size() - 1);
    return str;
}

// Covert all characters in string to lowercase
inline std::string to_lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](const unsigned char ch) { return std::tolower(ch); });
    return s;
}
