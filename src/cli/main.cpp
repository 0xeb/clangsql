// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

/// @file main.cpp
/// @brief clangsql CLI - SQL interface for Clang AST

#include <clangsql/clangsql.hpp>
#include <clangsql/session.hpp>
#include <clangsql/compile_commands.hpp>
#include <clangsql/project.hpp>
#include <xsql/query_script.hpp>
#ifdef CLANGSQL_HAS_HTTP
#include "http_server.hpp"
#endif
#ifdef CLANGSQL_HAS_MCP
#include "mcp_server.hpp"
#endif
#include <iostream>
#include <string>
#include <vector>
#include <iomanip>
#include <thread>
#include <regex>
#include <set>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cctype>
#if defined(CLANGSQL_HAS_HTTP) || defined(CLANGSQL_HAS_MCP)
#include <csignal>
#include <mutex>
#endif

/// Extract just the program name from a path (handles both / and \)
std::string program_name(const char* path) {
    std::string p(path);
    auto pos = p.find_last_of("/\\");
    if (pos != std::string::npos) {
        return p.substr(pos + 1);
    }
    return p;
}

void print_usage(const char* argv0) {
    std::string prog = program_name(argv0);
    std::cerr << "clangsql " << clangsql::VERSION << " - SQL interface to Clang ASTs\n"
              << clangsql::COPYRIGHT << "\n\n"
              << "Usage:\n"
              << "  " << prog << " <files...> [options] [clang-args...]\n"
              << "\n"
              << "Local Options:\n"
              << "  -s, --source <path>  Source file (alternative to positional)\n"
              << "  -e <sql>           Execute SQL query and exit\n"
              << "  -i                 Interactive mode (REPL)\n"
#ifdef CLANGSQL_HAS_HTTP
              << "  --http [port]      Start HTTP REST server (default: 8080)\n"
#endif
#ifdef CLANGSQL_HAS_MCP
              << "  --mcp [port]       Start MCP server over SSE (default: random 9000-9999)\n"
#endif
#if defined(CLANGSQL_HAS_HTTP) || defined(CLANGSQL_HAS_MCP)
              << "  --bind <addr>      Bind address for server (default: 127.0.0.1)\n"
#endif
              << "  --token <token>    Auth token for HTTP server mode\n"
              << "  -h, --help         Show this help\n"
              << "  --version          Show version\n"
              << "\n"
              << "Project Options:\n"
              << "  --project <dir>            Parse entire directory (unified schema)\n"
              << "  --pattern <glob>           File patterns (default: *.c *.cpp for --project; no filter otherwise)\n"
              << "  --exclude <dir>            Directories to exclude (default: test,build for --project; no filter otherwise)\n"
              << "  --compile-commands <path>  Load compile_commands.json\n"
              << "  --build-dir <path>         Load from build directory\n"
              << "  'src/**/*.cpp'             Glob pattern for source files\n"
              << "\n"
              << "Cache Options:\n"
              << "  --cache                    Enable AST caching (faster re-parses)\n"
              << "  --no-cache                 Disable AST caching (default)\n"
              << "  --cache-dir <path>         Set cache directory\n"
              << "  --clear-cache              Clear all cached AST files\n"
              << "  --cache-verbose            Show cache hit/miss messages\n"
              << "\n"
              << "Files:\n"
              << "  file.cpp              Attach as schema 'file'\n"
              << "  file.cpp:myschema     Attach as schema 'myschema'\n"
              << "  Multiple files create schema-prefixed tables (e.g., main_functions)\n"
              << "\n"
              << "Clang args (auto-detected or after --):\n"
              << "  -I<path>, -D<name>, -std=c++XX, -isystem, -W*, -f*, etc.\n"
              << "\n"
              << "Tables (per schema):\n"
              << "  [schema_]files, [schema_]functions, [schema_]classes, [schema_]methods\n"
              << "  [schema_]fields, [schema_]variables, [schema_]parameters, [schema_]enums\n"
              << "  [schema_]calls, [schema_]inheritance\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << " main.cpp -e \"SELECT name FROM functions\"\n"
#ifdef CLANGSQL_HAS_HTTP
              << "  " << prog << " main.cpp --http 8080\n"
#endif
#ifdef CLANGSQL_HAS_MCP
              << "  " << prog << " main.cpp --mcp\n"
#endif
              ;
}

/// Check if argument looks like a source file
bool is_source_file(const std::string& arg) {
    // Check for common C/C++ extensions
    static const char* extensions[] = {
        ".cpp", ".cc", ".cxx", ".c++", ".C",
        ".c",
        ".hpp", ".hh", ".hxx", ".h++", ".H",
        ".h",
        nullptr
    };

    for (const char** ext = extensions; *ext; ++ext) {
        if (arg.size() > strlen(*ext) &&
            arg.compare(arg.size() - strlen(*ext), strlen(*ext), *ext) == 0) {
            return true;
        }
    }

    // Also check for file:schema syntax (handles Windows paths correctly)
    size_t last_sep = arg.find_last_of("/\\");
    size_t colon_search_start = (last_sep != std::string::npos) ? last_sep : 0;

    // Skip Windows drive letter (e.g., C:)
    if (arg.size() >= 2 && std::isalpha(arg[0]) && arg[1] == ':') {
        if (colon_search_start < 2) {
            colon_search_start = 2;
        }
    }

    size_t colon = arg.find(':', colon_search_start);
    if (colon != std::string::npos && colon > 0) {
        std::string file_part = arg.substr(0, colon);
        return is_source_file(file_part);
    }

    return false;
}

/// Check if argument looks like a glob pattern
bool is_glob_pattern(const std::string& arg) {
    return arg.find('*') != std::string::npos || arg.find('?') != std::string::npos;
}

/// Expand glob pattern to matching files
std::vector<std::string> expand_glob(const std::string& pattern) {
    std::vector<std::string> result;

    // Find the base directory (everything before the first wildcard)
    size_t first_wild = pattern.find_first_of("*?");
    if (first_wild == std::string::npos) {
        result.push_back(pattern);
        return result;
    }

    // Find last separator before wildcard
    size_t last_sep = pattern.find_last_of("/\\", first_wild);
    std::filesystem::path base_dir = (last_sep == std::string::npos)
        ? std::filesystem::current_path()
        : std::filesystem::path(pattern.substr(0, last_sep));

    // Get the pattern part
    std::string pattern_part = (last_sep == std::string::npos)
        ? pattern
        : pattern.substr(last_sep + 1);

    // Convert glob pattern to regex
    std::string regex_pattern;
    for (size_t i = 0; i < pattern_part.size(); ++i) {
        char c = pattern_part[i];
        if (c == '*') {
            if (i + 1 < pattern_part.size() && pattern_part[i + 1] == '*') {
                regex_pattern += ".*";  // ** matches anything including /
                ++i;
                if (i + 1 < pattern_part.size() && (pattern_part[i + 1] == '/' || pattern_part[i + 1] == '\\')) {
                    ++i;  // skip following separator
                }
            } else {
                regex_pattern += "[^/\\\\]*";  // * matches anything except separator
            }
        } else if (c == '?') {
            regex_pattern += "[^/\\\\]";
        } else if (c == '.' || c == '(' || c == ')' || c == '[' || c == ']' ||
                   c == '{' || c == '}' || c == '+' || c == '^' || c == '$' || c == '|') {
            regex_pattern += "\\";
            regex_pattern += c;
        } else {
            regex_pattern += c;
        }
    }
    regex_pattern += "$";

    try {
        std::regex re(regex_pattern, std::regex::icase);

        if (std::filesystem::exists(base_dir)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(base_dir)) {
                if (entry.is_regular_file()) {
                    std::string rel_path = std::filesystem::relative(entry.path(), base_dir).string();
                    std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
                    if (std::regex_match(rel_path, re)) {
                        result.push_back(entry.path().string());
                    }
                }
            }
        }
    } catch (const std::regex_error&) {
        result.push_back(pattern);
    }

    std::sort(result.begin(), result.end());
    return result;
}

/// Check if argument is a clangsql option (not a Clang arg)
bool is_clangsql_option(const std::string& arg) {
    return arg == "-e" || arg == "-q" || arg == "-i" ||
           arg == "-s" || arg == "--source" ||
           arg == "-h" || arg == "--help" ||
           arg == "--version" ||
           arg == "--http" || arg == "--bind" ||
           arg == "--token" ||
           arg == "--compile-commands" || arg == "--build-dir" ||
           arg == "--cache" || arg == "--no-cache" ||
           arg == "--cache-dir" || arg == "--clear-cache" ||
           arg == "--cache-verbose" ||
#ifdef CLANGSQL_HAS_MCP
           arg == "--mcp" ||
#endif
           false;
}

/// Check if a file path matches pattern/exclude filters.
/// patterns: glob patterns like "*.cpp" matched against filename only
///           (case-insensitive via clangsql::detail::matches_any_basename_glob).
/// excludes: directory names; if any component of the path matches case-insensitively,
///           the file is excluded. Case folding keeps behavior consistent on
///           case-insensitive filesystems (Windows, macOS default).
/// Returns true if the file should be included.
bool matches_filters(const std::string& filepath,
                     const std::vector<std::string>& patterns,
                     const std::vector<std::string>& excludes) {
    namespace fs = std::filesystem;
    fs::path p(filepath);
    std::string filename = p.filename().string();

    auto to_lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    // Check excludes first (match against any path component, case-insensitive)
    if (!excludes.empty()) {
        std::vector<std::string> excludes_lc;
        excludes_lc.reserve(excludes.size());
        for (const auto& exc : excludes) excludes_lc.push_back(to_lower(exc));

        for (auto it = p.begin(); it != p.end(); ++it) {
            std::string component_lc = to_lower(it->string());
            for (const auto& exc : excludes_lc) {
                if (component_lc == exc)
                    return false;
            }
        }
    }

    // Check patterns (match filename against glob patterns)
    if (!patterns.empty()) {
        if (!clangsql::detail::matches_any_basename_glob(filename, patterns)) {
            return false;
        }
    }

    return true;
}

/// Parse file:schema syntax, returns {file, schema}
/// Handles Windows paths correctly (C:/path/file.cpp:schema)
std::pair<std::string, std::string> parse_file_spec(const std::string& spec) {
    // Find the last colon that could be a schema separator
    // It must be after any path separators and not at position 1 (Windows drive letter)
    size_t last_sep = spec.find_last_of("/\\");
    size_t colon_search_start = (last_sep != std::string::npos) ? last_sep : 0;

    // On Windows, skip drive letter colon (e.g., C:)
    if (spec.size() >= 2 && std::isalpha(spec[0]) && spec[1] == ':') {
        if (colon_search_start < 2) {
            colon_search_start = 2;
        }
    }

    // Look for colon after the filename portion
    size_t colon = spec.find(':', colon_search_start);
    if (colon != std::string::npos && colon > 0 && colon < spec.size() - 1) {
        return {spec.substr(0, colon), spec.substr(colon + 1)};
    }
    return {spec, ""};
}

/// Extract schema name from file path (e.g., "main.cpp" -> "main")
std::string schema_from_path(const std::string& path) {
    // Find last path separator
    size_t last_sep = path.find_last_of("/\\");
    std::string filename = (last_sep == std::string::npos) ? path : path.substr(last_sep + 1);

    // Remove extension
    size_t dot = filename.find_last_of('.');
    if (dot != std::string::npos) {
        filename = filename.substr(0, dot);
    }

    // Replace invalid chars with underscore
    for (char& c : filename) {
        if (!std::isalnum(c) && c != '_') {
            c = '_';
        }
    }

    return filename;
}

// Forward declarations
static std::string json_escape(const std::string& s);
static std::string query_result_to_json(const xsql::Result& result);
// Emit the results[] script envelope: the HTTP ?format=text|csv|tsv reformatter
// only understands that shape (the flat query_result_to_json() renders empty there).
static std::string session_query_to_script_json(clangsql::Session& session,
                                                 const std::string& sql);

/// Print result in tabular format
void print_result(const xsql::Result& result) {
    if (!result.ok()) {
        std::cerr << "Error: " << result.error << "\n";
        return;
    }

    if (result.empty()) {
        std::cout << "(no rows)\n";
        return;
    }

    // Calculate column widths
    std::vector<size_t> widths(result.columns.size());
    for (size_t i = 0; i < result.columns.size(); ++i) {
        widths[i] = result.columns[i].size();
    }
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = (std::max)(widths[i], row[i].size());
        }
    }

    // Print header
    for (size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0) std::cout << " | ";
        std::cout << std::left << std::setw(static_cast<int>(widths[i])) << result.columns[i];
    }
    std::cout << "\n";

    // Print separator
    for (size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0) std::cout << "-+-";
        std::cout << std::string(widths[i], '-');
    }
    std::cout << "\n";

    // Print rows
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            if (i > 0) std::cout << " | ";
            std::cout << std::left << std::setw(static_cast<int>(widths[i])) << row[i];
        }
        std::cout << "\n";
    }

    std::cout << "(" << result.size() << " row" << (result.size() != 1 ? "s" : "") << ")\n";
}


/// Run interactive REPL
void run_repl(clangsql::Session& session) {
    std::cout << "clangsql " << clangsql::VERSION << " - Interactive Mode\n";
    std::cout << clangsql::COPYRIGHT << "\n";
    std::cout << "Type .help for help, .clear to reset, .quit to exit\n\n";

    std::string line;
    std::string buffer;

    while (true) {
        std::cout << (buffer.empty() ? "clangsql> " : "     ...> ");
        if (!std::getline(std::cin, line)) {
            break;
        }

        // Handle dot commands
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            if (line == ".quit" || line == ".exit") {
                break;
            } else if (line == ".help") {
                std::cout << "Commands:\n"
                          << "  .tables       List all tables\n"
                          << "  .schema <t>   Show table schema\n"
                          << "  .attached     List attached TUs\n"
                          << "  .quit         Exit\n";
            } else if (line == ".tables") {
                auto result = session.query("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
                for (const auto& row : result.rows) {
                    std::cout << "  " << row[0] << "\n";
                }
            } else if (line.substr(0, 8) == ".schema ") {
                std::string table = line.substr(8);
                auto result = session.query("PRAGMA table_info(" + table + ")");
                if (result.ok() && !result.empty()) {
                    std::cout << "CREATE TABLE " << table << " (\n";
                    for (size_t i = 0; i < result.rows.size(); ++i) {
                        const auto& row = result.rows[i];
                        std::cout << "  " << row[1] << " " << row[2];
                        if (i + 1 < result.rows.size()) std::cout << ",";
                        std::cout << "\n";
                    }
                    std::cout << ");\n";
                } else {
                    std::cout << "Table not found: " << table << "\n";
                }
            } else if (line == ".attached") {
                for (const auto& schema : session.attached_schemas()) {
                    std::cout << "  " << schema << "\n";
                }
            } else {
                std::cout << "Unknown command: " << line << "\n";
            }
            continue;
        }

        // Accumulate multi-line SQL
        buffer += line + " ";

        // Check if statement is complete (ends with semicolon)
        size_t pos = buffer.find_last_not_of(" \t\n\r");
        if (pos != std::string::npos && buffer[pos] == ';') {
            // Execute the query
            auto result = session.query(buffer);
            print_result(result);
            buffer.clear();
        }
    }

    std::cout << "\nGoodbye!\n";
}

//=============================================================================
// HTTP Server Mode
//=============================================================================

#ifdef CLANGSQL_HAS_HTTP
static clangsql::ClangsqlHTTPServer* g_http_server = nullptr;

static void http_signal_handler(int) {
    if (g_http_server) g_http_server->stop();
}
#endif  // CLANGSQL_HAS_HTTP

// JSON serialization helpers are shared by the HTTP and MCP server modes.
#if defined(CLANGSQL_HAS_HTTP) || defined(CLANGSQL_HAS_MCP)
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 10);
    for (char ch : s) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

static std::string query_result_to_json(const xsql::Result& result) {
    std::ostringstream json;
    json << "{";
    json << "\"success\":" << (result.ok() ? "true" : "false");

    if (result.ok()) {
        json << ",\"columns\":[";
        for (size_t i = 0; i < result.columns.size(); i++) {
            if (i > 0) json << ",";
            json << "\"" << json_escape(result.columns[i]) << "\"";
        }
        json << "]";

        json << ",\"rows\":[";
        for (size_t i = 0; i < result.rows.size(); i++) {
            if (i > 0) json << ",";
            json << "[";
            for (size_t c = 0; c < result.rows[i].size(); c++) {
                if (c > 0) json << ",";
                // Honor the per-cell SQL-NULL flag: a real NULL serializes to JSON
                // null (distinct from a genuine text value, incl. "" or "NULL").
                if (result.rows[i].is_null(c)) {
                    json << "null";
                } else {
                    json << "\"" << json_escape(result.rows[i][c]) << "\"";
                }
            }
            json << "]";
        }
        json << "]";
        json << ",\"row_count\":" << result.rows.size();
    } else {
        json << ",\"error\":\"" << json_escape(result.error) << "\"";
    }

    json << "}";
    return json.str();
}

// Run `sql` as a (possibly multi-statement) script and serialize it as the
// canonical xsql envelope {success,statement_count,results:[...],...}. Carries
// per-cell SQL-NULL fidelity so ?format=json and the text/csv/tsv reformatter
// agree on NULLs. Shared by every HTTP query callback (REPL .http + --http).
static std::string session_query_to_script_json(clangsql::Session& session,
                                                 const std::string& sql) {
    auto script = xsql::run_script(sql, {},
        [&session](const std::string& stmt, xsql::ScriptStatementResult& out) {
            auto r = session.query(stmt);
            out.columns = r.columns;
            out.rows.reserve(r.rows.size());
            out.cell_null.reserve(r.rows.size());
            for (const auto& row : r.rows) {
                out.rows.push_back(row.values);
                out.cell_null.push_back(row.nulls);   // carry SQL-NULL fidelity
            }
            out.elapsed_ms = static_cast<double>(r.elapsed_ms);
            out.success = r.error.empty();
            out.error = r.error;
        });
    return xsql::script_result_to_json(script);
}
#endif  // CLANGSQL_HAS_HTTP || CLANGSQL_HAS_MCP

#ifdef CLANGSQL_HAS_HTTP
static const char* CLANGSQL_HELP_TEXT = R"(CLANGSQL HTTP REST API
======================

SQL interface for Clang AST via HTTP.

Endpoints:
  GET  /         - Welcome message
  GET  /help     - This documentation (for LLM discovery)
  POST /query    - Execute SQL (body = raw SQL, response = JSON)
  GET  /status   - Server health
  POST /shutdown - Stop server

Tables (per schema):
  [schema_]files       - Source files
  [schema_]functions   - Functions and methods
  [schema_]classes     - Classes, structs, unions
  [schema_]methods     - Class methods
  [schema_]fields      - Class/struct fields
  [schema_]variables   - Variables (global, local)
  [schema_]parameters  - Function parameters
  [schema_]enums       - Enumerations
  [schema_]calls       - Function call sites
  [schema_]inheritance - Class inheritance

Example Queries:
  SELECT name, return_type FROM functions WHERE is_virtual = 1;
  SELECT name, kind FROM classes;
  SELECT caller, callee FROM calls;

Response Format:
  Success: {"success": true, "columns": [...], "rows": [[...]], "row_count": N}
  Error:   {"success": false, "error": "message"}

Authentication (if enabled):
  Header: Authorization: Bearer <token>
  Or:     X-XSQL-Token: <token>

Example:
  curl http://localhost:8080/help
  curl -X POST http://localhost:8080/query -d "SELECT name FROM functions LIMIT 5"
)";

int run_http_mode(clangsql::Session& session, int port, const std::string& bind_addr, const std::string& auth_token) {
    std::string actual_bind = bind_addr.empty() ? "127.0.0.1" : bind_addr;

    if (!bind_addr.empty() && bind_addr != "127.0.0.1" && bind_addr != "localhost") {
        std::cerr << "WARNING: Binding to non-loopback address " << bind_addr << "\n";
        if (auth_token.empty()) {
            std::cerr << "WARNING: No authentication token set. Server is accessible without authentication.\n";
            std::cerr << "         Consider using --token <secret> for remote access.\n";
        }
    }

    std::mutex query_mutex;
    clangsql::ClangsqlHTTPServer http_server;

    auto query_cb = [&session, &query_mutex](const std::string& sql) -> std::string {
        std::lock_guard<std::mutex> lock(query_mutex);
        return session_query_to_script_json(session, sql);
    };

    int actual_port = http_server.start(port, query_cb, actual_bind, false);
    if (actual_port <= 0) {
        std::cerr << "Error: Failed to start HTTP server on port " << port << "\n";
        return 1;
    }

    g_http_server = &http_server;
    auto old_handler = std::signal(SIGINT, http_signal_handler);

    std::cout << "HTTP server listening on http://" << actual_bind << ":" << actual_port << "\n";
    std::cout << "Endpoints: /help, /query, /status, /shutdown\n";
    std::cout << "Example: curl http://localhost:" << actual_port << "/help\n";
    std::cout << "Press Ctrl+C to stop.\n\n";
    std::cout.flush();

    // Wait for server to stop (Ctrl+C or /shutdown)
    while (http_server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::signal(SIGINT, old_handler);
    g_http_server = nullptr;
    std::cout << "\nHTTP server stopped.\n";
    return 0;
}
#endif // CLANGSQL_HAS_HTTP

//=============================================================================
// MCP Server Mode
//=============================================================================

#ifdef CLANGSQL_HAS_MCP
static clangsql::ClangsqlMCPServer* g_mcp_mode_server = nullptr;

static void mcp_signal_handler(int) {
    if (g_mcp_mode_server) g_mcp_mode_server->stop();
}

// Serve the clangsql_query MCP tool over SSE until Ctrl+C. The server calls the
// query callback on its own thread, so guard session access with a mutex.
int run_mcp_mode(clangsql::Session& session, int port, const std::string& bind_addr) {
    std::string actual_bind = bind_addr.empty() ? "127.0.0.1" : bind_addr;

    if (!actual_bind.empty() && actual_bind != "127.0.0.1" && actual_bind != "localhost") {
        std::cerr << "WARNING: Binding MCP server to non-loopback address " << actual_bind << "\n";
    }

    std::mutex query_mutex;
    clangsql::ClangsqlMCPServer mcp_server;

    clangsql::QueryCallback query_cb = [&session, &query_mutex](const std::string& sql) -> std::string {
        std::lock_guard<std::mutex> lock(query_mutex);
        return session_query_to_script_json(session, sql);
    };

    int actual_port = mcp_server.start(port, query_cb, actual_bind, false);
    if (actual_port <= 0) {
        std::cerr << "Error: Failed to start MCP server\n";
        return 1;
    }

    g_mcp_mode_server = &mcp_server;
    auto old_handler = std::signal(SIGINT, mcp_signal_handler);

    std::cout << clangsql::format_mcp_info(actual_port);
    std::cout << "Press Ctrl+C to stop.\n\n";
    std::cout.flush();

    // Wait for server to stop (Ctrl+C)
    while (mcp_server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::signal(SIGINT, old_handler);
    g_mcp_mode_server = nullptr;
    std::cout << "\nMCP server stopped.\n";
    return 0;
}
#endif // CLANGSQL_HAS_MCP

//=============================================================================
// Main
//=============================================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Collect source files, clang args, and clangsql options
    std::vector<std::pair<std::string, std::string>> source_files;  // {path, schema}
    std::vector<std::string> clang_args;
    std::string query;
    std::string auth_token;
    std::string bind_addr;
    int http_port = 8080;
    bool interactive = false;
    bool http_mode = false;
    bool mcp_mode = false;
    int mcp_port = 0;  // 0 = random port in 9000-9999
    bool after_dashdash = false;
    std::string compile_commands_path;
    std::string build_dir_path;

    // Project mode options
    std::string project_path;
    std::vector<std::string> project_patterns;
    std::vector<std::string> project_excludes;
    bool project_mode = false;

    // Cache options
    bool cache_enabled = false;
    bool cache_verbose = false;
    bool clear_cache = false;
    std::string cache_dir_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // After --, everything goes to Clang
        if (after_dashdash) {
            clang_args.push_back(arg);
            continue;
        }

        if (arg == "--") {
            after_dashdash = true;
        } else if ((arg == "-e" || arg == "-q") && i + 1 < argc) {
            query = argv[++i];
        } else if (arg == "-i") {
            interactive = true;
        } else if (arg == "--http") {
            http_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    http_port = std::stoi(argv[++i]);
                } catch (...) {
                    std::cerr << "Invalid HTTP port number\n";
                    return 1;
                }
            }
        } else if (arg == "--mcp") {
            mcp_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    mcp_port = std::stoi(argv[++i]);
                } catch (...) {
                    std::cerr << "Invalid MCP port number\n";
                    return 1;
                }
            }
        } else if (arg == "--bind" && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (arg == "--token" && i + 1 < argc) {
            auth_token = argv[++i];
        } else if (arg == "--compile-commands" && i + 1 < argc) {
            compile_commands_path = argv[++i];
        } else if (arg == "--build-dir" && i + 1 < argc) {
            build_dir_path = argv[++i];
        } else if (arg == "--project" && i + 1 < argc) {
            project_path = argv[++i];
            project_mode = true;
        } else if (arg == "--pattern" && i + 1 < argc) {
            project_patterns.push_back(argv[++i]);
        } else if (arg == "--exclude" && i + 1 < argc) {
            project_excludes.push_back(argv[++i]);
        } else if (arg == "--cache") {
            cache_enabled = true;
        } else if (arg == "--no-cache") {
            cache_enabled = false;
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            cache_dir_path = argv[++i];
            cache_enabled = true;  // Implicitly enable if dir specified
        } else if (arg == "--clear-cache") {
            clear_cache = true;
        } else if (arg == "--cache-verbose") {
            cache_verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--version") {
            std::cout << "clangsql " << clangsql::VERSION << "\n" << clangsql::COPYRIGHT << "\n";
            return 0;
        } else if ((arg == "-s" || arg == "--source") && i + 1 < argc) {
            // Explicit source file option
            std::string src = argv[++i];
            auto [path, schema] = parse_file_spec(src);
            if (schema.empty()) {
                schema = schema_from_path(path);
            }
            source_files.push_back({path, schema});
        } else if (is_glob_pattern(arg)) {
            // Expand glob pattern
            auto matches = expand_glob(arg);
            for (const auto& match : matches) {
                if (is_source_file(match)) {
                    source_files.push_back({match, schema_from_path(match)});
                }
            }
        } else if (is_source_file(arg)) {
            // Parse file:schema syntax
            auto [path, schema] = parse_file_spec(arg);
            if (schema.empty()) {
                schema = schema_from_path(path);
            }
            source_files.push_back({path, schema});
        } else if (arg[0] == '-') {
            // Assume it's a Clang arg
            clang_args.push_back(arg);
            // Handle args that take a value (like -I /path)
            if ((arg == "-I" || arg == "-D" || arg == "-isystem" ||
                 arg == "-include" || arg == "-x") && i + 1 < argc) {
                clang_args.push_back(argv[++i]);
            }
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    //=========================================================================
    // Load compile_commands.json if specified
    //=========================================================================
    clangsql::CompileCommandsDatabase compile_db;
    if (!compile_commands_path.empty()) {
        if (!compile_db.load(compile_commands_path)) {
            std::cerr << "Error: Failed to load " << compile_commands_path << "\n";
            return 1;
        }
        std::cerr << "Loaded " << compile_db.size() << " compile commands\n";
        for (const auto& cmd : compile_db.commands()) {
            if (is_source_file(cmd.file) &&
                matches_filters(cmd.file, project_patterns, project_excludes)) {
                source_files.push_back({cmd.file, schema_from_path(cmd.file)});
            }
        }
    } else if (!build_dir_path.empty()) {
        if (!compile_db.load_from_directory(build_dir_path)) {
            std::cerr << "Error: No compile_commands.json in " << build_dir_path << "\n";
            return 1;
        }
        std::cerr << "Loaded " << compile_db.size() << " compile commands\n";
        for (const auto& cmd : compile_db.commands()) {
            if (is_source_file(cmd.file) &&
                matches_filters(cmd.file, project_patterns, project_excludes)) {
                source_files.push_back({cmd.file, schema_from_path(cmd.file)});
            }
        }
    }

    //=========================================================================
    // Project mode - discover and parse all files in directory
    //=========================================================================
    if (project_mode) {
        clangsql::ProjectConfig config;
        config.root_path = project_path;

        // Use provided patterns or defaults
        if (!project_patterns.empty()) {
            config.patterns = project_patterns;
        }

        // Use provided excludes or defaults
        if (!project_excludes.empty()) {
            config.exclude = project_excludes;
        }

        // Add include paths from clang_args
        for (size_t i = 0; i < clang_args.size(); ++i) {
            if (clang_args[i].substr(0, 2) == "-I") {
                if (clang_args[i].size() > 2) {
                    config.include_paths.push_back(clang_args[i].substr(2));
                } else if (i + 1 < clang_args.size()) {
                    config.include_paths.push_back(clang_args[++i]);
                }
            } else if (clang_args[i].substr(0, 2) == "-D") {
                if (clang_args[i].size() > 2) {
                    config.defines.push_back(clang_args[i].substr(2));
                } else if (i + 1 < clang_args.size()) {
                    config.defines.push_back(clang_args[++i]);
                }
            } else if (clang_args[i].substr(0, 5) == "-std=") {
                config.std_version = clang_args[i].substr(5);
            }
        }

        clangsql::Project project = clangsql::Project::load(config);

        if (!project.valid()) {
            std::cerr << "Error: " << project.error() << "\n";
            return 1;
        }

        std::cerr << "Project: " << project_path << "\n";
        std::cerr << "Found " << project.file_count() << " source files\n";

        // Add all project files with empty schema (unified mode)
        for (const auto& file : project.source_files()) {
            source_files.push_back({file, ""});  // Empty schema = unified
        }
    }

    //=========================================================================
    // Local modes - require source files
    //=========================================================================
    if (source_files.empty() && !project_mode) {
        std::cerr << "Error: No source files specified\n";
        print_usage(argv[0]);
        return 1;
    }

    // Create session
    clangsql::Session session;

    // Configure caching
    if (!cache_dir_path.empty()) {
        // Custom cache directory specified
        session.set_cache_dir(cache_dir_path);
    }
    session.set_caching_enabled(cache_enabled);
    session.set_cache_verbose(cache_verbose);

    // Handle --clear-cache
    if (clear_cache) {
        session.clear_ast_cache();
        std::cerr << "Cache cleared (" << session.ast_cache().cache_dir().string() << ")" << std::endl;
    }

    // Set default args from CLI
    if (!clang_args.empty()) {
        session.set_default_args(clang_args);
    }

    //=========================================================================
    // Project mode - parse all files and register unified tables
    //=========================================================================
    if (project_mode) {
        // Parse all project files into separate TUs
        std::vector<std::unique_ptr<clangsql::TranslationUnit>> parsed_tus;
        std::vector<const clangsql::TranslationUnit*> tu_ptrs;
        int success_count = 0;
        int fail_count = 0;

        for (const auto& [path, schema] : source_files) {
            std::cerr << "Parsing " << path << "...\n";

            auto tu = std::make_unique<clangsql::TranslationUnit>();
            if (tu->parse(session.index(), path, clang_args)) {
                tu_ptrs.push_back(tu.get());
                parsed_tus.push_back(std::move(tu));
                success_count++;
            } else {
                std::cerr << "  Warning: Failed to parse (skipped)\n";
                fail_count++;
            }
        }

        std::cerr << "\nParsed " << success_count << " files";
        if (fail_count > 0) {
            std::cerr << " (" << fail_count << " failed)";
        }
        std::cerr << "\n";

        if (tu_ptrs.empty()) {
            std::cerr << "Error: No files parsed successfully\n";
            return 1;
        }

        // Register unified tables from all TUs
        clangsql::register_project_tables(session.database(), tu_ptrs, "");
        std::cerr << "Registered unified tables\n\n";
    }
    //=========================================================================
    // Normal mode - attach individual files
    //=========================================================================
    else {
        bool single_file = (source_files.size() == 1);
        for (const auto& [path, schema] : source_files) {
            std::cerr << "Parsing " << path << "...\n";

            // For single file, use empty schema (no prefix)
            std::string effective_schema = single_file ? "" : schema;

            if (!session.attach(path, effective_schema)) {
                std::cerr << "Error: " << session.last_error() << "\n";
                return 1;
            }

            if (single_file) {
                std::cerr << "Attached (no prefix)\n";
            } else {
                std::cerr << "Attached as: " << schema << "_*\n";
            }
        }
        std::cerr << "\n";
    }

#ifdef CLANGSQL_HAS_HTTP
    if (http_mode) {
        return run_http_mode(session, http_port, bind_addr, auth_token);
    }
#else
    if (http_mode) {
        std::cerr << "Error: HTTP mode not available. Rebuild with -DCLANGSQL_WITH_HTTP=ON\n";
        return 1;
    }
#endif

#ifdef CLANGSQL_HAS_MCP
    if (mcp_mode) {
        return run_mcp_mode(session, mcp_port, bind_addr);
    }
#else
    if (mcp_mode) {
        std::cerr << "Error: MCP mode not available. Rebuild with -DCLANGSQL_WITH_MCP=ON\n";
        return 1;
    }
#endif

    //=========================================================================
    // Local query/interactive mode
    //=========================================================================

    // Execute query if specified
    if (!query.empty()) {
        auto result = session.query(query);
        print_result(result);
        return result.ok() ? 0 : 1;
    }

    // Interactive mode
    if (interactive || query.empty()) {
        run_repl(session);
    }

    return 0;
}
