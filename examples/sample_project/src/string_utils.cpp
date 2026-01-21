/// @file string_utils.cpp
/// @brief Implementation of string utilities

#include <sample/string_utils.hpp>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace sample {
namespace strings {

StringUtils::StringUtils(const StringConfig& config)
    : config_(config) {}

std::string StringUtils::to_upper(const std::string& str) const {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

std::string StringUtils::to_lower(const std::string& str) const {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string StringUtils::trim(const std::string& str) const {
    auto start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

std::vector<std::string> StringUtils::split(const std::string& str) const {
    std::vector<std::string> result;
    std::string current;
    for (char c : str) {
        if (c == config_.delimiter) {
            if (config_.trim_whitespace) {
                current = trim(current);
            }
            if (!current.empty()) {
                result.push_back(current);
            }
            current.clear();
        } else {
            current += c;
        }
    }
    if (config_.trim_whitespace) {
        current = trim(current);
    }
    if (!current.empty()) {
        result.push_back(current);
    }
    return result;
}

std::string StringUtils::join(const std::vector<std::string>& parts) const {
    if (parts.empty()) return "";
    std::string result = parts[0];
    for (size_t i = 1; i < parts.size(); ++i) {
        result += config_.delimiter;
        result += parts[i];
    }
    return result;
}

bool StringUtils::contains(const std::string& str, const std::string& substr) const {
    if (config_.case_sensitive) {
        return str.find(substr) != std::string::npos;
    }
    std::string lower_str = to_lower(str);
    std::string lower_substr = to_lower(substr);
    return lower_str.find(lower_substr) != std::string::npos;
}

std::string reverse(const std::string& str) {
    return std::string(str.rbegin(), str.rend());
}

bool starts_with(const std::string& str, const std::string& prefix) {
    if (prefix.size() > str.size()) return false;
    return str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace strings
} // namespace sample
