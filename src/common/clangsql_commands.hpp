// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <functional>
#include <string>
#include <sstream>

#ifdef CLANGSQL_HAS_AI_AGENT
#include "agent_settings.hpp"
#endif

namespace clangsql {

/**
 * Command handler result
 */
enum class CommandResult {
    NOT_HANDLED,  // Not a command, process as query
    HANDLED,      // Command executed successfully
    QUIT          // User requested quit
};

/**
 * Command handler callbacks
 *
 * These callbacks allow different environments (CLI, plugin) to extend
 * command behavior. For example, .clear might:
 *   - Core: Reset the AI agent session
 *   - Plugin: Also clear the UI output window
 */
struct CommandCallbacks {
    std::function<std::string()> get_tables;      // Return table list
    std::function<std::string(const std::string&)> get_schema;  // Return schema for table
    std::function<std::string()> get_info;        // Return database info
    std::function<std::string()> clear_session;   // Clear/reset session (agent, UI, etc.)

    // MCP server callbacks (optional)
    std::function<std::string()> mcp_status;      // Get MCP status
    std::function<std::string()> mcp_start;       // Start MCP server
    std::function<std::string()> mcp_stop;        // Stop MCP server

#ifdef CLANGSQL_HAS_HTTP
    // HTTP server callbacks (optional)
    std::function<std::string()> http_status;     // Get HTTP server status
    std::function<std::string()> http_start;      // Start HTTP server
    std::function<std::string()> http_stop;       // Stop HTTP server
#endif
};

/**
 * Handle dot commands (.tables, .schema, .help, .quit, etc.)
 *
 * @param input User input line
 * @param callbacks Callbacks to execute commands
 * @param output Output string (filled if command produces output)
 * @return CommandResult indicating how to proceed
 */
inline CommandResult handle_command(
    const std::string& input,
    const CommandCallbacks& callbacks,
    std::string& output)
{
    if (input.empty() || input[0] != '.') {
        return CommandResult::NOT_HANDLED;
    }

    if (input == ".quit" || input == ".exit") {
        return CommandResult::QUIT;
    }

    if (input == ".tables") {
        if (callbacks.get_tables) {
            output = callbacks.get_tables();
        }
        return CommandResult::HANDLED;
    }

    if (input == ".info") {
        if (callbacks.get_info) {
            output = callbacks.get_info();
        }
        return CommandResult::HANDLED;
    }

    if (input == ".clear") {
        if (callbacks.clear_session) {
            output = callbacks.clear_session();
        } else {
            output = "Session cleared";
        }
        return CommandResult::HANDLED;
    }

    if (input == ".help") {
        output = "CLANGSQL Commands:\n"
                 "  .tables         List all tables\n"
                 "  .schema <table> Show table schema\n"
                 "  .info           Show database info\n"
                 "  .clear          Clear/reset session\n"
                 "  .quit / .exit   Exit\n"
                 "  .help           Show this help\n"
#ifdef CLANGSQL_HAS_AI_AGENT
                 "\n"
                 "MCP Server:\n"
                 "  .mcp            Show status or start if not running\n"
                 "  .mcp start      Start MCP server\n"
                 "  .mcp stop       Stop MCP server\n"
                 "  .mcp help       Show MCP help\n"
#endif
#ifdef CLANGSQL_HAS_HTTP
                 "\n"
                 "HTTP Server:\n"
                 "  .http           Show status or start if not running\n"
                 "  .http start     Start HTTP server\n"
                 "  .http stop      Stop HTTP server\n"
                 "  .http help      Show HTTP help\n"
#endif
                 "\n"
                 "SQL:\n"
                 "  SELECT * FROM functions LIMIT 10;\n"
                 "  SELECT name, return_type FROM functions ORDER BY name;\n"
#ifdef CLANGSQL_HAS_AI_AGENT
                 "\n"
                 "AI Agent:\n"
                 "  .agent help       Show agent commands\n"
                 "  .agent provider   Show/set AI provider\n"
                 "  .agent clear      Clear conversation\n"
                 "\n"
                 "Natural Language:\n"
                 "  Find all classes that inherit from Base\n"
                 "  Show functions that call malloc\n"
                 "  List all methods in the MyClass class\n"
#endif
                 ;
        return CommandResult::HANDLED;
    }

    // .mcp commands (MCP server control)
    if (input.rfind(".mcp", 0) == 0) {
#ifdef CLANGSQL_HAS_AI_AGENT
        std::string subargs = input.length() > 4 ? input.substr(4) : "";
        // Trim leading whitespace
        size_t start = subargs.find_first_not_of(" \t");
        if (start != std::string::npos)
            subargs = subargs.substr(start);

        if (subargs.empty()) {
            // .mcp - show status, start if not running
            if (callbacks.mcp_status) {
                output = callbacks.mcp_status();
            } else {
                output = "MCP server not available";
            }
        }
        else if (subargs == "start") {
            if (callbacks.mcp_start) {
                output = callbacks.mcp_start();
            } else {
                output = "MCP server not available";
            }
        }
        else if (subargs == "stop") {
            if (callbacks.mcp_stop) {
                output = callbacks.mcp_stop();
            } else {
                output = "MCP server not available";
            }
        }
        else if (subargs == "help") {
            output = "MCP Server Commands:\n"
                     "  .mcp            Show status, start if not running\n"
                     "  .mcp start      Start MCP server on random port\n"
                     "  .mcp stop       Stop MCP server\n"
                     "  .mcp help       Show this help\n"
                     "\n"
                     "The MCP server exposes two tools:\n"
                     "  clangsql_query  - Execute SQL query directly\n"
                     "  clangsql_agent  - Ask natural language question (AI-powered)\n"
                     "\n"
                     "Connect with Claude Desktop by adding to config:\n"
                     "  {\"mcpServers\": {\"clangsql\": {\"url\": \"http://127.0.0.1:<port>/sse\"}}}\n";
        }
        else {
            output = "Unknown MCP command: " + subargs + "\nUse '.mcp help' for available commands.";
        }
#else
        output = "MCP server requires AI agent support. Rebuild with -DCLANGSQL_WITH_AI_AGENT=ON";
#endif
        return CommandResult::HANDLED;
    }

    // .http commands (HTTP server control)
    if (input.rfind(".http", 0) == 0) {
#ifdef CLANGSQL_HAS_HTTP
        std::string subargs = input.length() > 5 ? input.substr(5) : "";
        // Trim leading whitespace
        size_t start = subargs.find_first_not_of(" \t");
        if (start != std::string::npos)
            subargs = subargs.substr(start);

        if (subargs.empty()) {
            // .http - show status, start if not running
            if (callbacks.http_status) {
                output = callbacks.http_status();
            } else {
                output = "HTTP server not available";
            }
        }
        else if (subargs == "start") {
            if (callbacks.http_start) {
                output = callbacks.http_start();
            } else {
                output = "HTTP server not available";
            }
        }
        else if (subargs == "stop") {
            if (callbacks.http_stop) {
                output = callbacks.http_stop();
            } else {
                output = "HTTP server not available";
            }
        }
        else if (subargs == "help") {
            output = "HTTP Server Commands:\n"
                     "  .http            Show status, start if not running\n"
                     "  .http start      Start HTTP server on random port\n"
                     "  .http stop       Stop HTTP server\n"
                     "  .http help       Show this help\n"
                     "\n"
                     "Endpoints:\n"
                     "  GET  /help       API documentation\n"
                     "  POST /query      Execute SQL (body = raw SQL)\n"
                     "  GET  /status     Health check\n"
                     "  POST /shutdown   Stop server\n"
                     "\n"
                     "Example:\n"
                     "  curl -X POST http://127.0.0.1:<port>/query -d \"SELECT name FROM functions LIMIT 5\"\n";
        }
        else {
            output = "Unknown HTTP command: " + subargs + "\nUse '.http help' for available commands.";
        }
#else
        output = "HTTP server not compiled in. Rebuild with -DCLANGSQL_WITH_HTTP=ON";
#endif
        return CommandResult::HANDLED;
    }

    // .agent commands
    if (input.rfind(".agent", 0) == 0) {
#ifdef CLANGSQL_HAS_AI_AGENT
        std::string subargs = input.length() > 6 ? input.substr(6) : "";
        // Trim leading whitespace
        size_t start = subargs.find_first_not_of(" \t");
        if (start != std::string::npos)
            subargs = subargs.substr(start);

        // Parse subcmd and value
        std::string subcmd, value;
        size_t space = subargs.find(' ');
        if (space != std::string::npos) {
            subcmd = subargs.substr(0, space);
            value = subargs.substr(space + 1);
            size_t val_start = value.find_first_not_of(" \t");
            if (val_start != std::string::npos)
                value = value.substr(val_start);
        } else {
            subcmd = subargs;
        }

        auto settings = LoadAgentSettings();
        std::string provider_name = libagents::provider_type_name(settings.default_provider);

        if (subcmd.empty() || subcmd == "help") {
            output = "Agent Commands:\n"
                     "  .agent help               Show this help\n"
                     "  .agent provider           Show current provider\n"
                     "  .agent provider NAME      Switch provider (claude, copilot)\n"
                     "  .agent clear              Clear conversation\n"
                     "  .agent timeout            Show response timeout\n"
                     "  .agent timeout MS         Set response timeout in milliseconds\n"
                     "  .agent byok               Show BYOK status\n"
                     "  .agent byok enable        Enable BYOK\n"
                     "  .agent byok disable       Disable BYOK\n"
                     "  .agent byok key VALUE     Set API key\n"
                     "  .agent byok endpoint URL  Set API endpoint\n"
                     "  .agent byok model NAME    Set model name\n"
                     "  .agent byok type TYPE     Set provider type (openai, anthropic, azure)\n"
                     "\nCurrent provider: " + provider_name + "\n";
        }
        else if (subcmd == "provider") {
            if (value.empty()) {
                output = "Current provider: " + provider_name + "\n"
                         "\nAvailable providers:\n"
                         "  claude   - Claude Code (Anthropic)\n"
                         "  copilot  - GitHub Copilot\n";
            } else {
                try {
                    auto type = ParseProviderType(value);
                    settings.default_provider = type;
                    SaveAgentSettings(settings);
                    output = "Provider set to: " + std::string(libagents::provider_type_name(type)) +
                             " (saved to settings)\n"
                             "Note: Restart agent session for changes to take effect.\n";
                } catch (const std::exception& e) {
                    output = std::string("Error: ") + e.what() + "\n"
                             "Available providers: claude, copilot\n";
                }
            }
        }
        else if (subcmd == "clear") {
            if (callbacks.clear_session) {
                output = callbacks.clear_session();
            } else {
                output = "Session cleared";
            }
        }
        else if (subcmd == "timeout") {
            if (value.empty()) {
                output = "Response timeout: " + std::to_string(settings.response_timeout_ms) + " ms (" +
                         std::to_string(settings.response_timeout_ms / 1000) + " seconds)\n";
            } else {
                try {
                    int ms = std::stoi(value);
                    if (ms < 1000) {
                        output = "Error: Timeout must be at least 1000 ms (1 second).\n";
                    } else {
                        settings.response_timeout_ms = ms;
                        SaveAgentSettings(settings);
                        output = "Timeout set to " + std::to_string(ms) + " ms (" +
                                 std::to_string(ms / 1000) + " seconds).\n";
                    }
                } catch (...) {
                    output = "Error: Invalid timeout value. Use milliseconds.\n";
                }
            }
        }
        else if (subcmd == "byok") {
            // Parse BYOK subcommand
            std::string byok_subcmd, byok_value;
            size_t byok_space = value.find(' ');
            if (byok_space != std::string::npos) {
                byok_subcmd = value.substr(0, byok_space);
                byok_value = value.substr(byok_space + 1);
                size_t bv_start = byok_value.find_first_not_of(" \t");
                if (bv_start != std::string::npos)
                    byok_value = byok_value.substr(bv_start);
            } else {
                byok_subcmd = value;
            }

            const BYOKSettings* byok = settings.get_byok();

            if (byok_subcmd.empty()) {
                std::stringstream ss;
                ss << "BYOK status for provider '" << provider_name << "':\n";
                if (byok) {
                    ss << "  Enabled:  " << (byok->enabled ? "yes" : "no") << "\n"
                       << "  API Key:  " << (byok->api_key.empty() ? "(not set)" : "********") << "\n"
                       << "  Endpoint: " << (byok->base_url.empty() ? "(default)" : byok->base_url) << "\n"
                       << "  Model:    " << (byok->model.empty() ? "(default)" : byok->model) << "\n"
                       << "  Type:     " << (byok->provider_type.empty() ? "(default)" : byok->provider_type) << "\n"
                       << "  Usable:   " << (byok->is_usable() ? "yes" : "no") << "\n";
                } else {
                    ss << "  (not configured)\n";
                }
                output = ss.str();
            }
            else if (byok_subcmd == "enable") {
                auto& b = settings.get_or_create_byok();
                b.enabled = true;
                SaveAgentSettings(settings);
                output = "BYOK enabled for provider '" + provider_name + "'.\n";
                if (b.api_key.empty()) {
                    output += "Warning: API key not set. Use '.agent byok key <value>' to set it.\n";
                }
            }
            else if (byok_subcmd == "disable") {
                auto& b = settings.get_or_create_byok();
                b.enabled = false;
                SaveAgentSettings(settings);
                output = "BYOK disabled for provider '" + provider_name + "'.\n";
            }
            else if (byok_subcmd == "key") {
                if (byok_value.empty()) {
                    output = "Error: API key value required.\n"
                             "Usage: .agent byok key <value>\n";
                } else {
                    auto& b = settings.get_or_create_byok();
                    b.api_key = byok_value;
                    SaveAgentSettings(settings);
                    output = "BYOK API key set for provider '" + provider_name + "'.\n";
                }
            }
            else if (byok_subcmd == "endpoint") {
                auto& b = settings.get_or_create_byok();
                b.base_url = byok_value;
                SaveAgentSettings(settings);
                output = byok_value.empty() ?
                    "BYOK endpoint cleared (using default).\n" :
                    "BYOK endpoint set to: " + byok_value + "\n";
            }
            else if (byok_subcmd == "model") {
                auto& b = settings.get_or_create_byok();
                b.model = byok_value;
                SaveAgentSettings(settings);
                output = byok_value.empty() ?
                    "BYOK model cleared (using default).\n" :
                    "BYOK model set to: " + byok_value + "\n";
            }
            else if (byok_subcmd == "type") {
                auto& b = settings.get_or_create_byok();
                b.provider_type = byok_value;
                SaveAgentSettings(settings);
                output = byok_value.empty() ?
                    "BYOK type cleared (using default).\n" :
                    "BYOK type set to: " + byok_value + "\n";
            }
            else {
                output = "Unknown byok subcommand: " + byok_subcmd + "\n"
                         "Use '.agent byok' to see available commands.\n";
            }
        }
        else {
            output = "Unknown agent subcommand: " + subcmd + "\n"
                     "Use '.agent help' for available commands.\n";
        }
#else
        output = "AI agent support not compiled in. Rebuild with -DCLANGSQL_WITH_AI_AGENT=ON\n";
#endif
        return CommandResult::HANDLED;
    }

    if (input.rfind(".schema", 0) == 0) {
        std::string table = input.length() > 8 ? input.substr(8) : "";
        // Trim leading whitespace
        size_t start = table.find_first_not_of(" \t");
        if (start != std::string::npos) {
            table = table.substr(start);
            // Trim trailing whitespace
            size_t end = table.find_last_not_of(" \t");
            if (end != std::string::npos) {
                table = table.substr(0, end + 1);
            }
        } else {
            table.clear();
        }

        if (table.empty()) {
            output = "Usage: .schema <table_name>";
        } else if (callbacks.get_schema) {
            output = callbacks.get_schema(table);
        }
        return CommandResult::HANDLED;
    }

    output = "Unknown command: " + input;
    return CommandResult::HANDLED;
}

} // namespace clangsql
