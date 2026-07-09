// Copyright (c) 2024-2026 Elias Bachaalany
// SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
//
// This file is licensed under the Human-Origin Source License v1.0.
// See LICENSE.

#pragma once
#include "types.h"

namespace demo {

class Connection {
public:
    Connection(const char* host, int port);
    ~Connection();

    bool connect();
    void disconnect();

    // Security: returns raw pointer (ownership unclear)
    char* receive(int* out_len);

    // Security: takes raw buffer without size
    bool send(const char* data);

    // Security: many parameters
    bool send_with_options(const char* data, int len, int flags,
                          int timeout, bool retry, int max_retries,
                          void* context);

    bool is_connected() const { return connected_; }

private:
    char* host_;      // Security: raw pointer member
    int port_;
    int socket_fd_;
    bool connected_;
    char buffer_[1024];
};

// Security: global mutable state
extern Connection* g_default_connection;
extern int g_connection_count;

// Factory
Connection* create_connection(const Config& config);

} // namespace demo
