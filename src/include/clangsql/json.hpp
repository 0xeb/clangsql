// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
