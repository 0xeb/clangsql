#pragma once
/// @file compile_commands.hpp
/// @brief compile_commands.json parser for clangsql

#include <clangsql/json.hpp>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>

namespace clangsql {

// C++17 compatibility helpers (starts_with/ends_with are C++20)
namespace detail {
    inline bool starts_with(const std::string& str, const std::string& prefix) {
        return str.size() >= prefix.size() && str.compare(0, prefix.size(), prefix) == 0;
    }
    inline bool ends_with(const std::string& str, const std::string& suffix) {
        return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
}

/// A single entry from compile_commands.json
struct CompileCommand {
    std::string directory;    ///< Working directory for the compilation
    std::string file;         ///< Source file path
    std::string command;      ///< Full compilation command (if present)
    std::vector<std::string> arguments;  ///< Command arguments (if present)

    /// Extract compiler flags (include paths, defines, std version, etc.)
    /// Handles both GCC/Clang and MSVC command line formats
    std::vector<std::string> extract_flags() const {
        using detail::starts_with;
        using detail::ends_with;

        std::vector<std::string> flags;
        const auto& args = arguments.empty() ? split_command(command) : arguments;

        for (size_t i = 0; i < args.size(); ++i) {
            const std::string& arg = args[i];

            // Skip compiler and source file
            if (i == 0) continue;  // compiler
            if (arg == file) continue;
            if (ends_with(arg, ".cpp") || ends_with(arg, ".c") ||
                ends_with(arg, ".cc") || ends_with(arg, ".cxx")) continue;
            // Skip output/object file specifications
            if (starts_with(arg, "/Fo") || starts_with(arg, "/Fd") ||
                starts_with(arg, "-o")) continue;

            // GCC/Clang style flags
            if (starts_with(arg, "-I") && arg.size() > 2) {
                flags.push_back(arg);
            } else if (arg == "-I" && i + 1 < args.size()) {
                flags.push_back("-I" + args[++i]);
            } else if (starts_with(arg, "-D") && arg.size() > 2) {
                flags.push_back(arg);
            } else if (arg == "-D" && i + 1 < args.size()) {
                flags.push_back("-D" + args[++i]);
            } else if (starts_with(arg, "-std=")) {
                flags.push_back(arg);
            } else if (starts_with(arg, "-isystem")) {
                if (arg.size() > 8) {
                    flags.push_back(arg);
                } else if (i + 1 < args.size()) {
                    flags.push_back("-isystem" + args[++i]);
                }
            }
            // MSVC style flags - convert to Clang format
            else if (starts_with(arg, "/I")) {
                if (arg.size() > 2) {
                    flags.push_back("-I" + arg.substr(2));
                } else if (i + 1 < args.size()) {
                    flags.push_back("-I" + args[++i]);
                }
            } else if (starts_with(arg, "/D")) {
                if (arg.size() > 2) {
                    flags.push_back("-D" + arg.substr(2));
                } else if (i + 1 < args.size()) {
                    flags.push_back("-D" + args[++i]);
                }
            } else if (starts_with(arg, "-std:")) {
                // MSVC uses -std:c++17, Clang uses -std=c++17
                flags.push_back("-std=" + arg.substr(5));
            }
            // Skip MSVC-specific flags that libclang doesn't understand
            // /nologo, /TP, /EHsc, /GR, /Zi, /Od, /RTC1, /MD, /FS, etc.
        }
        return flags;
    }

private:
    static std::vector<std::string> split_command(const std::string& cmd) {
        std::vector<std::string> result;
        std::string current;
        bool in_quote = false;
        char quote_char = 0;

        for (char c : cmd) {
            if (in_quote) {
                if (c == quote_char) {
                    in_quote = false;
                } else {
                    current += c;
                }
            } else if (c == '"' || c == '\'') {
                in_quote = true;
                quote_char = c;
            } else if (c == ' ' || c == '\t') {
                if (!current.empty()) {
                    result.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            result.push_back(current);
        }
        return result;
    }
};

/// Collection of compile commands from a compile_commands.json file
class CompileCommandsDatabase {
public:
    CompileCommandsDatabase() = default;

    /// Load from a compile_commands.json file
    bool load(const std::filesystem::path& json_path) {
        std::ifstream file(json_path);
        if (!file) return false;

        try {
            json j = json::parse(file);
            if (!j.is_array()) return false;

            commands_.clear();
            for (const auto& entry : j) {
                CompileCommand cmd;
                cmd.directory = entry.value("directory", "");
                cmd.file = entry.value("file", "");

                if (entry.contains("command")) {
                    cmd.command = entry["command"].get<std::string>();
                }
                if (entry.contains("arguments") && entry["arguments"].is_array()) {
                    for (const auto& arg : entry["arguments"]) {
                        cmd.arguments.push_back(arg.get<std::string>());
                    }
                }

                commands_.push_back(std::move(cmd));
            }
            return true;
        } catch (const json::exception&) {
            return false;
        }
    }

    /// Load from a directory containing compile_commands.json
    bool load_from_directory(const std::filesystem::path& dir) {
        return load(dir / "compile_commands.json");
    }

    /// Find compile command for a source file
    std::optional<CompileCommand> find(const std::filesystem::path& file) const {
        auto canonical = std::filesystem::weakly_canonical(file);
        for (const auto& cmd : commands_) {
            // Resolve cmd.file relative to cmd.directory for proper matching
            std::filesystem::path cmd_file = cmd.file;
            if (cmd_file.is_relative() && !cmd.directory.empty()) {
                cmd_file = std::filesystem::path(cmd.directory) / cmd_file;
            }
            auto cmd_canonical = std::filesystem::weakly_canonical(cmd_file);
            if (cmd_canonical == canonical) {
                return cmd;
            }
        }
        return std::nullopt;
    }

    /// Get all commands
    const std::vector<CompileCommand>& commands() const { return commands_; }

    /// Check if database is empty
    bool empty() const { return commands_.empty(); }

    /// Get number of entries
    size_t size() const { return commands_.size(); }

private:
    std::vector<CompileCommand> commands_;
};

} // namespace clangsql
