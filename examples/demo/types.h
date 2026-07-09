// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

#pragma once
#include <string>
#include <vector>

namespace demo {

// Value object
struct Config {
    std::string host;
    int port;
    int timeout_ms;
    bool use_ssl;
};

// Abstract base for handlers
class Handler {
public:
    virtual ~Handler() = default;
    virtual bool process(const char* data, size_t len) = 0;
    virtual const char* name() const = 0;
};

// Result type
struct Result {
    int code;
    std::string message;
    void* payload;  // Security: raw void pointer
};

} // namespace demo
