/// @file main.cpp
/// @brief clangsql CLI - SQL interface for Clang AST

#include <clangsql/clangsql.hpp>
#include <clangsql/session.hpp>
#include <clangsql/compile_commands.hpp>
#include <xsql/socket/server.hpp>
#include <xsql/socket/client.hpp>
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

#ifdef CLANGSQL_HAS_AI_AGENT
#include "ai_agent.hpp"
#include "clangsql_commands.hpp"
#include <csignal>
#include <atomic>

// Global agent pointer for signal handler
static std::atomic<clangsql::AIAgent*> g_agent{nullptr};

// Signal handler for Ctrl-C
void signal_handler(int signum) {
    if (signum == SIGINT) {
        auto* agent = g_agent.load();
        if (agent) {
            agent->request_quit();
        }
    }
}
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
    std::cerr << "Usage:\n"
              << "  " << prog << " <files...> [options] [clang-args...]\n"
              << "  " << prog << " --remote host:port [options]\n"
              << "\n"
              << "Local Options:\n"
              << "  -e <sql>           Execute SQL query and exit\n"
              << "  -i                 Interactive mode (REPL)\n"
#ifdef CLANGSQL_HAS_AI_AGENT
              << "  --agent            Enable AI agent mode\n"
              << "  --prompt <text>    Single-shot agent prompt\n"
              << "  --provider <name>  AI provider (claude, copilot)\n"
              << "  -v                 Verbose agent output\n"
#endif
              << "  --server [port]    Start server mode (default: " << clangsql::DEFAULT_PORT << ")\n"
              << "  --token <token>    Auth token for server mode\n"
              << "  -h, --help         Show this help\n"
              << "  --version          Show version\n"
              << "\n"
              << "Project Options:\n"
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
              << "Remote Options:\n"
              << "  --remote host:port Connect to remote server\n"
              << "  -q <sql>           Execute SQL query (remote)\n"
              << "  -i                 Interactive mode (remote)\n"
              << "  --token <token>    Auth token for remote connection\n"
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
#ifdef CLANGSQL_HAS_AI_AGENT
              << "  " << prog << " main.cpp --agent -i\n"
              << "  " << prog << " main.cpp --prompt \"Find all virtual methods\"\n"
#endif
              << "  " << prog << " main.cpp --server\n"
              << "  " << prog << " --remote localhost:" << clangsql::DEFAULT_PORT << " -q \"SELECT * FROM functions\"\n"
              << "  " << prog << " --remote localhost:" << clangsql::DEFAULT_PORT << " -i\n";
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
        : pattern.substr(0, last_sep);

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
           arg == "-h" || arg == "--help" ||
           arg == "--version" || arg == "--server" ||
           arg == "--remote" || arg == "--token" ||
           arg == "--compile-commands" || arg == "--build-dir" ||
           arg == "--cache" || arg == "--no-cache" ||
           arg == "--cache-dir" || arg == "--clear-cache" ||
           arg == "--cache-verbose" ||
#ifdef CLANGSQL_HAS_AI_AGENT
           arg == "--agent" || arg == "--prompt" ||
           arg == "--provider" || arg == "-v" ||
#endif
           false;
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

#ifdef CLANGSQL_HAS_AI_AGENT
/// SQL executor for AI agent
std::string execute_sql_for_agent(clangsql::Session& session, const std::string& sql) {
    auto result = session.query(sql);
    if (!result.ok()) {
        return "Error: " + result.error;
    }

    if (result.empty()) {
        return "(no rows)";
    }

    // Format as text table
    std::ostringstream oss;

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
        if (i > 0) oss << " | ";
        oss << std::left << std::setw(static_cast<int>(widths[i])) << result.columns[i];
    }
    oss << "\n";

    // Print separator
    for (size_t i = 0; i < result.columns.size(); ++i) {
        if (i > 0) oss << "-+-";
        oss << std::string(widths[i], '-');
    }
    oss << "\n";

    // Print rows
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            if (i > 0) oss << " | ";
            oss << std::left << std::setw(static_cast<int>(widths[i])) << row[i];
        }
        oss << "\n";
    }

    oss << "(" << result.size() << " row" << (result.size() != 1 ? "s" : "") << ")";
    return oss.str();
}

/// Run agent-enabled REPL
void run_agent_repl(clangsql::Session& session, clangsql::AIAgent& agent) {
    std::cout << "clangsql " << clangsql::VERSION << " - AI Agent Mode\n";
    std::cout << "Type .help for help, .quit to exit\n";
    std::cout << "You can use natural language or SQL queries.\n\n";

    // Install signal handler
    g_agent.store(&agent);
    std::signal(SIGINT, signal_handler);

    std::string line;
    std::string buffer;

    while (!agent.quit_requested()) {
        std::cout << (buffer.empty() ? "clangsql> " : "     ...> ");
        std::cout.flush();

        if (!std::getline(std::cin, line)) {
            break;
        }

        // Handle dot commands
        if (buffer.empty() && !line.empty() && line[0] == '.') {
            clangsql::CommandCallbacks callbacks;
            callbacks.get_tables = [&session]() {
                auto result = session.query("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
                std::ostringstream oss;
                for (const auto& row : result.rows) {
                    oss << "  " << row[0] << "\n";
                }
                return oss.str();
            };
            callbacks.get_schema = [&session](const std::string& table) {
                auto result = session.query("PRAGMA table_info(" + table + ")");
                if (!result.ok() || result.empty()) {
                    return std::string("Table not found: " + table);
                }
                std::ostringstream oss;
                oss << "CREATE TABLE " << table << " (\n";
                for (size_t i = 0; i < result.rows.size(); ++i) {
                    const auto& row = result.rows[i];
                    oss << "  " << row[1] << " " << row[2];
                    if (i + 1 < result.rows.size()) oss << ",";
                    oss << "\n";
                }
                oss << ");";
                return oss.str();
            };
            callbacks.clear_session = [&agent]() {
                agent.reset_session();
                return std::string("Agent session cleared");
            };

            std::string output;
            auto cmd_result = clangsql::handle_command(line, callbacks, output);

            if (cmd_result == clangsql::CommandResult::QUIT) {
                break;
            } else if (cmd_result == clangsql::CommandResult::HANDLED) {
                if (!output.empty()) {
                    std::cout << output << "\n";
                }
                continue;
            }
        }

        // Accumulate multi-line input
        buffer += line + " ";

        // Check if complete (ends with semicolon or looks like complete natural language)
        size_t pos = buffer.find_last_not_of(" \t\n\r");
        bool is_complete = false;

        if (pos != std::string::npos) {
            if (buffer[pos] == ';') {
                is_complete = true;
            } else if (!clangsql::AIAgent::looks_like_sql(buffer)) {
                // Natural language - consider complete if we have a newline
                is_complete = true;
            }
        }

        if (is_complete) {
            // Send to agent with streaming
            std::string response = agent.query_streaming(buffer, [](const std::string& delta) {
                std::cout << delta << std::flush;
            });
            std::cout << "\n\n";
            buffer.clear();
        }
    }

    // Restore default signal handler
    g_agent.store(nullptr);
    std::signal(SIGINT, SIG_DFL);

    std::cout << "\nGoodbye!\n";
}
#endif

/// Run interactive REPL
void run_repl(clangsql::Session& session) {
    std::cout << "clangsql " << clangsql::VERSION << " - Interactive Mode\n";
    std::cout << "Type .help for help, .quit to exit\n\n";

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
// Remote Mode Functions
//=============================================================================

/// Parse host:port string
std::pair<std::string, int> parse_host_port(const std::string& spec) {
    auto colon = spec.rfind(':');
    if (colon == std::string::npos) {
        return {spec, clangsql::DEFAULT_PORT};
    }
    return {spec.substr(0, colon), std::stoi(spec.substr(colon + 1))};
}

/// Print remote query result
void print_remote_result(const xsql::socket::RemoteResult& result) {
    if (!result.success) {
        std::cerr << "Error: " << result.error << "\n";
        return;
    }

    if (result.rows.empty() && result.columns.empty()) {
        std::cout << "OK\n";
        return;
    }

    if (result.rows.empty()) {
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

    std::cout << "(" << result.rows.size() << " row" << (result.rows.size() != 1 ? "s" : "") << ")\n";
}

/// Run remote interactive REPL
int run_remote_repl(xsql::socket::Client& client) {
    std::cout << "clangsql " << clangsql::VERSION << " - Remote Interactive Mode\n";
    std::cout << "Type .help for help, .quit to exit\n\n";

    std::string line;
    std::string buffer;

    while (true) {
        std::cout << (buffer.empty() ? "clangsql> " : "     ...> ");
        std::cout.flush();
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
                          << "  .quit         Exit\n";
            } else if (line == ".tables") {
                auto result = client.query("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
                if (result.success) {
                    for (const auto& row : result.rows) {
                        std::cout << "  " << row[0] << "\n";
                    }
                } else {
                    std::cerr << "Error: " << result.error << "\n";
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
            auto result = client.query(buffer);
            print_remote_result(result);
            buffer.clear();
        }
    }

    std::cout << "\nGoodbye!\n";
    return 0;
}

/// Run remote mode
int run_remote_mode(const std::string& remote_spec, const std::string& query,
                    const std::string& auth_token, bool interactive) {
    auto [host, port] = parse_host_port(remote_spec);

    std::cerr << "Connecting to " << host << ":" << port << "...\n";

    xsql::socket::Client client;
    if (!auth_token.empty()) {
        client.set_auth_token(auth_token);
    }

    if (!client.connect(host, port)) {
        std::cerr << "Connection failed: " << client.error() << "\n";
        return 1;
    }
    std::cerr << "Connected.\n\n";

    if (!query.empty()) {
        auto result = client.query(query);
        print_remote_result(result);
        return result.success ? 0 : 1;
    }

    if (interactive) {
        return run_remote_repl(client);
    }

    // No query and not interactive - just test connection
    std::cout << "Connection OK. Use -q or -i for queries.\n";
    return 0;
}

//=============================================================================
// Server Mode
//=============================================================================

int run_server_mode(clangsql::Session& session, int port, const std::string& auth_token) {
    xsql::socket::Server server;
    xsql::socket::ServerConfig cfg;
    cfg.port = port;
    cfg.verbose = false;  // We'll print our own messages
    if (!auth_token.empty()) {
        cfg.auth_token = auth_token;
    }
    server.set_config(cfg);

    server.set_query_handler([&session](const std::string& sql) -> xsql::socket::QueryResult {
        auto result = session.query(sql);

        xsql::socket::QueryResult qr;
        qr.success = result.ok();
        qr.error = result.error;
        qr.columns = result.columns;
        for (const auto& row : result.rows) {
            qr.rows.push_back(row.values);
        }
        return qr;
    });

    // Start server asynchronously to get actual port (useful when port=0)
    if (!server.run_async(port)) {
        std::cerr << "Failed to start server on port " << port << "\n";
        return 1;
    }

    int actual_port = server.port();
    std::cout << "PORT=" << actual_port << "\n";  // Machine-readable for scripts
    std::cerr << "clangsql server listening on port " << actual_port << "\n";
    std::cerr << "Connect with: clangsql --remote localhost:" << actual_port << " -q \"SELECT * FROM functions\"\n";
    std::cerr << "Press Ctrl+C to stop.\n\n";
    std::cerr.flush();
    std::cout.flush();

    // Wait for server to stop (Ctrl+C or external signal)
    while (server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}

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
    std::string remote_spec;
    std::string auth_token;
    int server_port = clangsql::DEFAULT_PORT;
    bool interactive = false;
    bool server_mode = false;
    bool after_dashdash = false;
    std::string compile_commands_path;
    std::string build_dir_path;

    // Cache options
    bool cache_enabled = false;
    bool cache_verbose = false;
    bool clear_cache = false;
    std::string cache_dir_path;

#ifdef CLANGSQL_HAS_AI_AGENT
    bool agent_mode = false;
    bool agent_verbose = false;
    std::string agent_prompt;
    std::string agent_provider;
#endif

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
        } else if (arg == "--server") {
            server_mode = true;
            // Optional port argument
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try {
                    server_port = std::stoi(argv[++i]);
                } catch (...) {
                    std::cerr << "Invalid port number\n";
                    return 1;
                }
            }
        } else if (arg == "--remote" && i + 1 < argc) {
            remote_spec = argv[++i];
        } else if (arg == "--token" && i + 1 < argc) {
            auth_token = argv[++i];
        } else if (arg == "--compile-commands" && i + 1 < argc) {
            compile_commands_path = argv[++i];
        } else if (arg == "--build-dir" && i + 1 < argc) {
            build_dir_path = argv[++i];
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
            std::cout << "clangsql " << clangsql::VERSION << "\n";
            return 0;
#ifdef CLANGSQL_HAS_AI_AGENT
        } else if (arg == "--agent") {
            agent_mode = true;
        } else if (arg == "--prompt" && i + 1 < argc) {
            agent_prompt = argv[++i];
            agent_mode = true;
        } else if (arg == "--provider" && i + 1 < argc) {
            agent_provider = argv[++i];
        } else if (arg == "-v") {
            agent_verbose = true;
#endif
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
    // Remote mode - thin client, NO libclang required
    //=========================================================================
    if (!remote_spec.empty()) {
        if (!source_files.empty()) {
            std::cerr << "Error: Cannot use both source files and --remote\n";
            return 1;
        }
        if (server_mode) {
            std::cerr << "Error: Cannot use both --server and --remote\n";
            return 1;
        }
        return run_remote_mode(remote_spec, query, auth_token, interactive);
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
            if (is_source_file(cmd.file)) {
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
            if (is_source_file(cmd.file)) {
                source_files.push_back({cmd.file, schema_from_path(cmd.file)});
            }
        }
    }

    //=========================================================================
    // Local modes - require source files
    //=========================================================================
    if (source_files.empty()) {
        std::cerr << "Error: No source files specified (or use --remote)\n";
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

    // Attach all source files
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

    //=========================================================================
    // Server mode
    //=========================================================================
    if (server_mode) {
        return run_server_mode(session, server_port, auth_token);
    }

    //=========================================================================
    // AI Agent Mode
    //=========================================================================
#ifdef CLANGSQL_HAS_AI_AGENT
    if (agent_mode) {
        // Load settings and apply provider override
        auto settings = clangsql::LoadAgentSettings();
        if (!agent_provider.empty()) {
            try {
                settings.default_provider = clangsql::ParseProviderType(agent_provider);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n";
                return 1;
            }
        }

        // Create agent
        auto executor = [&session](const std::string& sql) {
            return execute_sql_for_agent(session, sql);
        };
        clangsql::AIAgent agent(executor, settings, agent_verbose);

        // Start agent
        agent.start();
        if (!agent.is_available()) {
            std::cerr << "Error: AI agent not available. Check your provider configuration.\n";
            return 1;
        }

        // Single prompt mode
        if (!agent_prompt.empty()) {
            std::string response = agent.query_streaming(agent_prompt, [](const std::string& delta) {
                std::cout << delta << std::flush;
            });
            std::cout << "\n";
            return 0;
        }

        // Interactive agent mode
        run_agent_repl(session, agent);
        return 0;
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
