// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

#pragma once
/// @file json.hpp
/// @brief JSON library alias for clangsql
///
/// Re-exports xsql::json for use in clangsql.
/// The JSON library is provided by libxsql as a shared dependency.

#include <xsql/json.hpp>

namespace clangsql {

/// JSON type alias (re-export from xsql)
using json = xsql::json;

/// Ordered JSON (preserves insertion order)
using ordered_json = xsql::ordered_json;

} // namespace clangsql
