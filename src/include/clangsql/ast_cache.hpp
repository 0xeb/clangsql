// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
/// @file ast_cache.hpp
/// @brief AST caching for faster subsequent parses

#include <clangsql/parser.hpp>
#include <clang-c/Index.h>
#include <filesystem>
#include <iostream>
#include <cmath>
#include <string>
#include <vector>
#include <ctime>
#include <chrono>
#include <fstream>
#include <sstream>
#include <functional>
#include <optional>
#include <cstdlib>

namespace clangsql {

/// Get Clang version string (for cache invalidation)
inline std::string get_clang_version() {
    return to_string(clang_getClangVersion());
}

/// Simple hash function for strings (FNV-1a)
inline uint64_t hash_string(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

/// Cache metadata stored alongside AST file
struct CacheMetadata {
    std::string source_path;              // Original source file path
    std::time_t source_mtime = 0;         // Source file mtime at parse time
    std::string args_hash;                // Hash of compiler arguments
    std::string clang_version;            // Clang version used
    std::vector<std::pair<std::string, std::time_t>> includes;  // Include dependencies

    /// Serialize to a simple text format
    std::string serialize() const {
        std::ostringstream oss;
        oss << "source_path=" << source_path << "\n";
        oss << "source_mtime=" << source_mtime << "\n";
        oss << "args_hash=" << args_hash << "\n";
        oss << "clang_version=" << clang_version << "\n";
        oss << "include_count=" << includes.size() << "\n";
        for (const auto& [path, mtime] : includes) {
            oss << "include=" << path << "|" << mtime << "\n";
        }
        return oss.str();
    }

    /// Deserialize from text format
    static std::optional<CacheMetadata> deserialize(const std::string& data) {
        CacheMetadata meta;
        std::istringstream iss(data);
        std::string line;

        while (std::getline(iss, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            if (key == "source_path") {
                meta.source_path = value;
            } else if (key == "source_mtime") {
                meta.source_mtime = std::stoll(value);
            } else if (key == "args_hash") {
                meta.args_hash = value;
            } else if (key == "clang_version") {
                meta.clang_version = value;
            } else if (key == "include") {
                auto sep = value.find('|');
                if (sep != std::string::npos) {
                    std::string path = value.substr(0, sep);
                    std::time_t mtime = std::stoll(value.substr(sep + 1));
                    meta.includes.emplace_back(path, mtime);
                }
            }
        }

        if (meta.source_path.empty()) return std::nullopt;
        return meta;
    }
};

/// AST Cache Manager
/// Handles caching of parsed translation units to disk
class ASTCache {
public:
    /// Construct with cache directory
    /// @param cache_dir Directory to store cache files (default: ~/.cache/clangsql)
    explicit ASTCache(const std::filesystem::path& cache_dir = default_cache_dir())
        : cache_dir_(cache_dir) {
        std::filesystem::create_directories(cache_dir_);
    }

    /// Get default cache directory
    static std::filesystem::path default_cache_dir() {
#ifdef _WIN32
        if (const char* appdata = std::getenv("LOCALAPPDATA")) {
            return std::filesystem::path(appdata) / "clangsql" / "cache";
        }
        return std::filesystem::path(".clangsql_cache");
#else
        if (const char* home = std::getenv("HOME")) {
            return std::filesystem::path(home) / ".cache" / "clangsql";
        }
        return std::filesystem::path(".clangsql_cache");
#endif
    }

    /// Generate cache key from source path and args
    std::string cache_key(const std::string& source_path,
                          const std::vector<std::string>& args) const {
        // Normalize path
        std::filesystem::path abs_path = std::filesystem::absolute(source_path);

        // Hash args
        std::string args_combined;
        for (const auto& arg : args) {
            args_combined += arg + "\0";
        }
        uint64_t args_hash = hash_string(args_combined);

        // Create key from path hash + args hash
        uint64_t path_hash = hash_string(abs_path.string());

        std::ostringstream oss;
        oss << std::hex << path_hash << "_" << args_hash;
        return oss.str();
    }

    /// Get AST file path for a cache key
    std::filesystem::path ast_path(const std::string& key) const {
        return cache_dir_ / (key + ".ast");
    }

    /// Get metadata file path for a cache key
    std::filesystem::path meta_path(const std::string& key) const {
        return cache_dir_ / (key + ".meta");
    }

    /// Check if cache entry exists and is valid
    /// @param source_path Source file path
    /// @param args Compiler arguments
    /// @param verbose If true, print debug info to stderr
    /// @return true if cache is valid and can be used
    bool is_valid(const std::string& source_path,
                  const std::vector<std::string>& args,
                  bool verbose = false) const {
        std::string key = cache_key(source_path, args);

        // Check if files exist
        if (!std::filesystem::exists(ast_path(key)) ||
            !std::filesystem::exists(meta_path(key))) {
            // Cache files not found
            return false;
        }

        // Load metadata
        auto meta = load_metadata(key);
        if (!meta) {
            // Metadata load failed
            return false;
        }

        // Check Clang version
        if (meta->clang_version != get_clang_version()) {
            // Clang version mismatch - cache invalid
            return false;
        }

        // Check source file mtime (allow 2s tolerance for filesystem time resolution)
        constexpr std::time_t MTIME_TOLERANCE = 2;
        std::time_t current_mtime = get_file_mtime(source_path);
        if (std::abs(current_mtime - meta->source_mtime) > MTIME_TOLERANCE) {
            // Source file was modified
            return false;
        }

        // Check all include dependencies
        for (const auto& [inc_path, inc_mtime] : meta->includes) {
            std::time_t current_inc_mtime = get_file_mtime(inc_path);
            if (std::abs(current_inc_mtime - inc_mtime) > MTIME_TOLERANCE) {
                // Include file was modified
                return false;  // Include file changed
            }
        }

        return true;
    }

    /// Load a cached translation unit
    /// @param index libclang index
    /// @param source_path Source file path (for key generation)
    /// @param args Compiler arguments (for key generation)
    /// @param tu Output translation unit
    /// @return true on success
    bool load(CXIndex index, const std::string& source_path,
              const std::vector<std::string>& args, TranslationUnit& tu) const {
        std::string key = cache_key(source_path, args);
        std::filesystem::path ast = ast_path(key);

        if (!std::filesystem::exists(ast)) {
            return false;
        }

        return tu.load(index, ast.string());
    }

    /// Save a parsed translation unit to cache
    /// @param source_path Source file path
    /// @param args Compiler arguments
    /// @param tu Translation unit to cache
    /// @return true on success
    bool save(const std::string& source_path,
              const std::vector<std::string>& args,
              const TranslationUnit& tu) {
        std::string key = cache_key(source_path, args);

        // Save AST
        if (!tu.save(ast_path(key).string())) {
            return false;
        }

        // Build and save metadata
        CacheMetadata meta;
        meta.source_path = std::filesystem::absolute(source_path).string();
        meta.source_mtime = get_file_mtime(source_path);
        meta.args_hash = std::to_string(hash_string(
            [&]() {
                std::string combined;
                for (const auto& arg : args) combined += arg + "\0";
                return combined;
            }()
        ));
        meta.clang_version = get_clang_version();
        meta.includes = tu.get_inclusions();

        return save_metadata(key, meta);
    }

    /// Clear all cached entries
    void clear() {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_, ec)) {
            std::filesystem::remove(entry.path(), ec);
        }
    }

    /// Clear cache for a specific source file
    void clear(const std::string& source_path, const std::vector<std::string>& args) {
        std::string key = cache_key(source_path, args);
        std::error_code ec;
        std::filesystem::remove(ast_path(key), ec);
        std::filesystem::remove(meta_path(key), ec);
    }

    /// Get cache directory
    const std::filesystem::path& cache_dir() const { return cache_dir_; }

    /// Get cache statistics
    struct Stats {
        size_t entry_count = 0;
        size_t total_size_bytes = 0;
    };

    Stats stats() const {
        Stats s;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_, ec)) {
            if (entry.path().extension() == ".ast") {
                s.entry_count++;
                s.total_size_bytes += std::filesystem::file_size(entry.path(), ec);
            }
        }
        return s;
    }

private:
    std::filesystem::path cache_dir_;

    /// Get file modification time
    static std::time_t get_file_mtime(const std::string& path) {
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(path, ec);
        if (ec) return 0;

        // Convert file_time_type to time_t
        // We need to handle the epoch difference between file_clock and system_clock
        // On Windows, file_time_type uses FILETIME epoch (1601), not Unix epoch (1970)
        // The safest approach is to compute the offset once using reference times
        static const auto offset = []() {
            auto file_now = std::filesystem::file_time_type::clock::now();
            auto sys_now = std::chrono::system_clock::now();
            auto file_since_epoch = file_now.time_since_epoch();
            auto sys_since_epoch = sys_now.time_since_epoch();
            return std::chrono::duration_cast<std::chrono::seconds>(
                file_since_epoch - std::chrono::duration_cast<
                    std::filesystem::file_time_type::duration>(sys_since_epoch)
            ).count();
        }();

        auto duration = ftime.time_since_epoch();
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
        return static_cast<std::time_t>(seconds.count() - offset);
    }

    /// Load metadata from file
    std::optional<CacheMetadata> load_metadata(const std::string& key) const {
        std::ifstream ifs(meta_path(key));
        if (!ifs) return std::nullopt;

        std::ostringstream oss;
        oss << ifs.rdbuf();
        return CacheMetadata::deserialize(oss.str());
    }

    /// Save metadata to file
    bool save_metadata(const std::string& key, const CacheMetadata& meta) {
        std::ofstream ofs(meta_path(key));
        if (!ofs) return false;
        ofs << meta.serialize();
        return ofs.good();
    }
};

} // namespace clangsql
