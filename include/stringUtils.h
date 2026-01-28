#pragma once
#include <algorithm>
#include <string>

// Remove whitespace characters from both start and end of a string
inline std::string trim(const std::string& str) {
    auto start = std::find_if(str.begin(), str.end(), [](unsigned char ch) { return !isspace(ch); });
    auto end = std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) { return !isspace(ch); }).base();
    return (start < end) ? std::string(start, end) : std::string();
}

//  Remove trailing carriage return character ('\r') from string if existed
inline std::string rtrim_cc(const std::string& str) {
    if (!str.empty() && str.back() == '\r')
        return str.substr(0, str.size() - 1);
    return str;
}

// Covert all characters in string to lowercase

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return s;
}