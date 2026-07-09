// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

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
constexpr const char* VERSION = "0.0.5";

/// Copyright / attribution line shown in CLI output
constexpr const char* COPYRIGHT = "Copyright (c) 2024-2026 Elias Bachaalany";

} // namespace clangsql
