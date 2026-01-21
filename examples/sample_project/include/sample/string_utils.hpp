#pragma once
/// @file string_utils.hpp
/// @brief String manipulation utilities

#include <string>
#include <vector>

namespace sample {
namespace strings {

/// String manipulation result codes
enum class ResultCode {
    Success = 0,
    EmptyInput,
    InvalidFormat,
    OutOfRange
};

/// Configuration for string operations
struct StringConfig {
    bool case_sensitive = true;
    bool trim_whitespace = false;
    char delimiter = ',';
};

/// String utility class
class StringUtils {
public:
    explicit StringUtils(const StringConfig& config = {});

    /// Convert to uppercase
    std::string to_upper(const std::string& str) const;

    /// Convert to lowercase
    std::string to_lower(const std::string& str) const;

    /// Trim whitespace from both ends
    std::string trim(const std::string& str) const;

    /// Split string by delimiter
    std::vector<std::string> split(const std::string& str) const;

    /// Join strings with delimiter
    std::string join(const std::vector<std::string>& parts) const;

    /// Check if string contains substring
    bool contains(const std::string& str, const std::string& substr) const;

private:
    StringConfig config_;
};

/// Standalone utility functions
std::string reverse(const std::string& str);
bool starts_with(const std::string& str, const std::string& prefix);
bool ends_with(const std::string& str, const std::string& suffix);

} // namespace strings
} // namespace sample
