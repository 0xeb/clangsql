#pragma once
/// @file session.hpp
/// @brief SQL session management for clangsql

#include <xsql/database.hpp>
#include <clangsql/parser.hpp>
#include <clangsql/tables.hpp>
#include <clangsql/compile_commands.hpp>
#include <clangsql/ast_cache.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <sys/stat.h>
#include <iostream>

namespace clangsql {

/// Get file modification time (cross-platform)
inline std::time_t get_file_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

/// Translation unit cache entry
struct TUCacheEntry {
    std::unique_ptr<TranslationUnit> tu;
    std::string path;
    std::string schema_name;
    std::vector<std::string> args;
    std::time_t mtime = 0;  // For staleness detection
};

/// SQL session for clangsql
/// Manages TU cache, virtual table registration, and ATTACH semantics
class Session {
public:
    Session() : index_(false, true) {}  // displayDiagnostics = true

    /// Get the underlying xsql database
    xsql::Database& database() { return db_; }
    const xsql::Database& database() const { return db_; }

    /// Get the libclang index
    CXIndex index() const { return index_.get(); }

    // ========================================================================
    // AST Caching
    // ========================================================================

    /// Enable or disable AST caching
    void set_caching_enabled(bool enabled) { caching_enabled_ = enabled; }
    bool caching_enabled() const { return caching_enabled_; }

    /// Enable verbose cache messages
    void set_cache_verbose(bool verbose) { cache_verbose_ = verbose; }
    bool cache_verbose() const { return cache_verbose_; }

    /// Set cache directory (creates new cache with specified path)
    void set_cache_dir(const std::filesystem::path& cache_dir) {
        ast_cache_ = ASTCache(cache_dir);
    }

    /// Get the AST cache
    ASTCache& ast_cache() { return ast_cache_; }
    const ASTCache& ast_cache() const { return ast_cache_; }

    /// Clear all cached AST files
    void clear_ast_cache() { ast_cache_.clear(); }

    // ========================================================================
    // Default Arguments (applied to all subsequent attaches)
    // ========================================================================

    /// Set default compiler arguments for all subsequent attaches
    /// These are merged with per-attach args (per-attach args take precedence)
    void set_default_args(const std::vector<std::string>& args) {
        default_args_ = args;
    }

    /// Get the current default arguments
    const std::vector<std::string>& default_args() const {
        return default_args_;
    }

    /// Add a single default argument
    void add_default_arg(const std::string& arg) {
        default_args_.push_back(arg);
    }

    /// Clear all default arguments
    void clear_default_args() {
        default_args_.clear();
    }

    // ========================================================================
    // Compile Commands Database
    // ========================================================================

    /// Load compile_commands.json from a file path
    /// @param json_path Path to compile_commands.json
    /// @return true on success
    bool load_compile_commands(const std::filesystem::path& json_path) {
        if (!compile_commands_db_.load(json_path)) {
            last_error_ = "Failed to load compile_commands.json from '" + json_path.string() + "'";
            return false;
        }
        return true;
    }

    /// Load compile_commands.json from a build directory
    /// @param build_dir Directory containing compile_commands.json
    /// @return true on success
    bool load_compile_commands_from_directory(const std::filesystem::path& build_dir) {
        if (!compile_commands_db_.load_from_directory(build_dir)) {
            last_error_ = "Failed to load compile_commands.json from directory '" + build_dir.string() + "'";
            return false;
        }
        return true;
    }

    /// Check if compile commands database is loaded
    bool has_compile_commands() const {
        return !compile_commands_db_.empty();
    }

    /// Get the compile commands database
    const CompileCommandsDatabase& compile_commands() const {
        return compile_commands_db_;
    }

    /// Check if a schema is attached
    bool is_attached(const std::string& schema_name) const {
        return tu_cache_.find(schema_name) != tu_cache_.end();
    }

    /// Get list of attached schemas
    std::vector<std::string> attached_schemas() const {
        std::vector<std::string> result;
        result.reserve(tu_cache_.size());
        for (const auto& pair : tu_cache_) {
            result.push_back(pair.first);
        }
        return result;
    }

    /// Attach a translation unit with the given schema name
    /// @param path Source file path
    /// @param schema_name SQL schema prefix - tables will be named schema_tablename
    /// @param args Additional compiler arguments (merged with compile_commands and default_args)
    /// @return true on success
    ///
    /// Argument precedence (later overrides earlier):
    /// 1. compile_commands.json flags (if loaded and file found)
    /// 2. default_args (set via set_default_args)
    /// 3. per-attach args (passed to this function)
    bool attach(const std::string& path, const std::string& schema_name,
                const std::vector<std::string>& args = {}) {
        // Check if already attached
        if (is_attached(schema_name)) {
            last_error_ = "Schema '" + schema_name + "' already attached";
            return false;
        }

        // Build merged args with proper precedence
        std::vector<std::string> merged_args;

        // 1. First, try to get flags from compile_commands.json
        if (!compile_commands_db_.empty()) {
            auto cmd = compile_commands_db_.find(path);
            if (cmd) {
                auto cc_flags = cmd->extract_flags();
                merged_args.insert(merged_args.end(), cc_flags.begin(), cc_flags.end());
            }
        }

        // 2. Then add default args
        merged_args.insert(merged_args.end(), default_args_.begin(), default_args_.end());

        // 3. Finally, per-attach args (highest precedence)
        merged_args.insert(merged_args.end(), args.begin(), args.end());

        // Create TU cache entry
        TUCacheEntry entry;
        entry.tu = std::make_unique<TranslationUnit>();
        entry.path = path;
        entry.schema_name = schema_name;
        entry.args = merged_args;  // Store merged args for reload
        entry.mtime = get_file_mtime(path);

        // Try to load from AST cache first
        bool loaded_from_cache = false;
        if (caching_enabled_ && ast_cache_.is_valid(path, merged_args, cache_verbose_)) {
            if (ast_cache_.load(index_.get(), path, merged_args, *entry.tu)) {
                loaded_from_cache = true;
                if (cache_verbose_) {
                    std::cerr << "[CACHE] Loaded from cache: " << path << std::endl;
                }
            }
        }

        // Parse if not loaded from cache
        if (!loaded_from_cache) {
            if (!entry.tu->parse(index_.get(), path, merged_args)) {
                last_error_ = "Failed to parse '" + path + "'";
                return false;
            }

            // Save to cache for next time
            if (caching_enabled_) {
                if (ast_cache_.save(path, merged_args, *entry.tu)) {
                    if (cache_verbose_) {
                        std::cerr << "[CACHE] Saved to cache: " << path << std::endl;
                    }
                }
            }
        }

        // Register virtual tables with schema prefix (e.g., main_functions)
        register_tables(db_, *entry.tu, schema_name);

        // Add to cache
        tu_cache_[schema_name] = std::move(entry);

        return true;
    }

    /// Detach a translation unit
    /// Note: Virtual tables remain registered (SQLite limitation)
    bool detach(const std::string& schema_name) {
        auto it = tu_cache_.find(schema_name);
        if (it == tu_cache_.end()) {
            last_error_ = "Schema '" + schema_name + "' not attached";
            return false;
        }

        // Remove from cache (TU will be destroyed)
        tu_cache_.erase(it);
        return true;
    }

    /// Check if a TU is stale (source file modified since parse)
    bool is_stale(const std::string& schema_name) const {
        auto it = tu_cache_.find(schema_name);
        if (it == tu_cache_.end()) {
            return false;
        }

        std::time_t current_mtime = get_file_mtime(it->second.path);
        return current_mtime > it->second.mtime;
    }

    /// Reload a stale TU (re-parse and update tables)
    bool reload(const std::string& schema_name) {
        auto it = tu_cache_.find(schema_name);
        if (it == tu_cache_.end()) {
            last_error_ = "Schema '" + schema_name + "' not attached";
            return false;
        }

        // Store the entry info
        std::string path = it->second.path;
        std::vector<std::string> args = it->second.args;

        // Detach and re-attach
        // Note: This is a simple approach; virtual tables will be re-registered
        // A more sophisticated approach would reparse in-place
        detach(schema_name);
        return attach(path, schema_name, args);
    }

    /// Execute a SQL query
    xsql::Result query(const std::string& sql) {
        return db_.query(sql);
    }

    /// Execute SQL (no results)
    int exec(const std::string& sql) {
        return db_.exec(sql);
    }

    /// Get last error message
    const std::string& last_error() const { return last_error_; }

private:
    xsql::Database db_;
    Index index_;
    std::unordered_map<std::string, TUCacheEntry> tu_cache_;
    std::vector<std::string> default_args_;
    CompileCommandsDatabase compile_commands_db_;
    std::string last_error_;

    // AST caching
    ASTCache ast_cache_;
    bool caching_enabled_ = false;  // Disabled by default
    bool cache_verbose_ = false;
};

} // namespace clangsql
