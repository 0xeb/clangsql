#pragma once
/// @file clangsql.hpp
/// @brief Public API for clangsql - SQL interface for Clang AST
///
/// Part of the xsql family (idasql, pdbsql, clangsql)

#include <clangsql/json.hpp>
#include <clangsql/parser.hpp>
#include <clangsql/session.hpp>
#include <clangsql/tables.hpp>

namespace clangsql {

/// Library version
constexpr const char* VERSION = "0.1.0";

/// Default server port for clangsql server mode
constexpr int DEFAULT_PORT = 17198;

} // namespace clangsql
