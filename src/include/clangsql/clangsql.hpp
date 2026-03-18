#pragma once
/// @file clangsql.hpp
/// @brief Public API for clangsql - SQL interface for Clang AST
///
/// Part of the xsql project family

#include <clangsql/json.hpp>
#include <clangsql/parser.hpp>
#include <clangsql/session.hpp>
#include <clangsql/tables.hpp>

namespace clangsql {

/// Library version
constexpr const char* VERSION = "0.1.0";

} // namespace clangsql
