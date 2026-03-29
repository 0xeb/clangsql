// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
/// @file project.hpp
/// @brief Project-level parsing support for clangsql
///
/// Enables parsing entire directories of source files into a unified schema,
/// rather than per-file schema prefixes.

#include <string>
#include <vector>
#include <filesystem>
#include <regex>
#include <set>

namespace clangsql {

/// Configuration for project-level parsing
struct ProjectConfig {
    std::filesystem::path root_path;                    ///< Project root directory
    std::vector<std::string> patterns = {"*.c", "*.cpp", "*.cc", "*.cxx"};  ///< File patterns to include
    std::vector<std::string> exclude = {"test", "tests", "build", "examples", "third_party"};  ///< Directories to exclude
    std::vector<std::string> include_paths;             ///< Include paths (-I)
    std::vector<std::string> defines;                   ///< Preprocessor defines (-D)
    std::string std_version;                            ///< C/C++ standard (e.g., "c++17", "c11")
    bool strict = false;                                ///< Fail on any parse error?
};

/// Result of parsing a single file in project mode
struct ParseResult {
    std::string path;
    std::string status;         ///< "ok", "failed", "skipped"
    std::string error_message;  ///< Empty if ok
    int error_count = 0;
    int warning_count = 0;
    int parse_time_ms = 0;
};

/// Project-level source file discovery and configuration
class Project {
public:
    Project() = default;

    /// Load project from configuration
    /// Scans directories and builds file list
    static Project load(const ProjectConfig& config);

    /// Get list of source files discovered
    const std::vector<std::string>& source_files() const { return source_files_; }

    /// Get number of source files
    size_t file_count() const { return source_files_.size(); }

    /// Get compiler flags for a specific file
    /// Returns combined flags from project config
    std::vector<std::string> flags_for(const std::string& file) const;

    /// Get the project root path
    const std::filesystem::path& root_path() const { return config_.root_path; }

    /// Get the project configuration
    const ProjectConfig& config() const { return config_; }

    /// Check if project is valid (has files)
    bool valid() const { return !source_files_.empty(); }

    /// Get error message if loading failed
    const std::string& error() const { return error_; }

private:
    ProjectConfig config_;
    std::vector<std::string> source_files_;
    std::string error_;

    /// Check if a filename matches any of the patterns
    static bool matches_patterns(const std::string& filename,
                                  const std::vector<std::string>& patterns);

    /// Check if a path should be excluded
    static bool should_exclude(const std::filesystem::path& path,
                               const std::vector<std::string>& exclude);

    /// Convert glob pattern to regex
    static std::string glob_to_regex(const std::string& glob);
};

// ============================================================================
// Implementation (header-only for simplicity)
// ============================================================================

inline Project Project::load(const ProjectConfig& config) {
    Project project;
    project.config_ = config;

    if (!std::filesystem::exists(config.root_path)) {
        project.error_ = "Project root does not exist: " + config.root_path.string();
        return project;
    }

    if (!std::filesystem::is_directory(config.root_path)) {
        project.error_ = "Project root is not a directory: " + config.root_path.string();
        return project;
    }

    // Scan directory recursively
    std::set<std::string> files_set;  // Use set to avoid duplicates

    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                config.root_path,
                std::filesystem::directory_options::skip_permission_denied)) {

            if (!entry.is_regular_file()) continue;

            // Check if in excluded directory
            if (should_exclude(entry.path(), config.exclude)) continue;

            // Check if matches any pattern
            std::string filename = entry.path().filename().string();
            if (matches_patterns(filename, config.patterns)) {
                files_set.insert(entry.path().string());
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        project.error_ = "Failed to scan directory: " + std::string(e.what());
        return project;
    }

    // Convert to vector (sorted)
    project.source_files_.assign(files_set.begin(), files_set.end());

    return project;
}

inline std::vector<std::string> Project::flags_for(const std::string& /*file*/) const {
    std::vector<std::string> flags;

    // Add include paths
    for (const auto& inc : config_.include_paths) {
        flags.push_back("-I" + inc);
    }

    // Add defines
    for (const auto& def : config_.defines) {
        flags.push_back("-D" + def);
    }

    // Add standard version
    if (!config_.std_version.empty()) {
        flags.push_back("-std=" + config_.std_version);
    }

    return flags;
}

inline bool Project::matches_patterns(const std::string& filename,
                                       const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        try {
            std::regex re(glob_to_regex(pattern), std::regex::icase);
            if (std::regex_match(filename, re)) {
                return true;
            }
        } catch (const std::regex_error&) {
            // Invalid pattern, try simple suffix match
            if (pattern.size() > 1 && pattern[0] == '*') {
                std::string suffix = pattern.substr(1);
                if (filename.size() >= suffix.size() &&
                    filename.compare(filename.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

inline bool Project::should_exclude(const std::filesystem::path& path,
                                     const std::vector<std::string>& exclude) {
    // Check each component of the path
    for (const auto& component : path) {
        std::string name = component.string();
        for (const auto& ex : exclude) {
            if (name == ex) {
                return true;
            }
        }
    }
    return false;
}

inline std::string Project::glob_to_regex(const std::string& glob) {
    std::string regex;
    regex.reserve(glob.size() * 2);

    for (size_t i = 0; i < glob.size(); ++i) {
        char c = glob[i];
        switch (c) {
            case '*':
                if (i + 1 < glob.size() && glob[i + 1] == '*') {
                    regex += ".*";
                    ++i;
                } else {
                    regex += "[^/\\\\]*";
                }
                break;
            case '?':
                regex += "[^/\\\\]";
                break;
            case '.':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '+':
            case '^':
            case '$':
            case '|':
            case '\\':
                regex += '\\';
                regex += c;
                break;
            default:
                regex += c;
        }
    }
    regex += '$';

    return regex;
}

} // namespace clangsql
