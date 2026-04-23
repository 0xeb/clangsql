// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
constexpr const char* VERSION = "0.0.2";

} // namespace clangsql
