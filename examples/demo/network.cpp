#include "network.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace demo {

// Global state
Connection* g_default_connection = nullptr;
int g_connection_count = 0;

Connection::Connection(const char* host, int port)
    : port_(port), socket_fd_(-1), connected_(false) {
    // Security: strcpy without bounds check
    host_ = (char*)malloc(256);
    strcpy(host_, host);
    g_connection_count++;
}

Connection::~Connection() {
    disconnect();
    free(host_);
    g_connection_count--;
}

bool Connection::connect() {
    // Simulated connection
    socket_fd_ = 42;
    connected_ = true;
    return true;
}

void Connection::disconnect() {
    if (connected_) {
        socket_fd_ = -1;
        connected_ = false;
    }
}

char* Connection::receive(int* out_len) {
    // Security: returns internal buffer (dangling pointer risk)
    *out_len = 100;
    return buffer_;
}

bool Connection::send(const char* data) {
    // Security: no null check, no length validation
    printf("Sending: %s\n", data);
    return connected_;
}

bool Connection::send_with_options(const char* data, int len, int flags,
                                   int timeout, bool retry, int max_retries,
                                   void* context) {
    // Too many parameters - code smell
    (void)flags;
    (void)timeout;
    (void)retry;
    (void)max_retries;
    (void)context;

    if (!connected_) return false;

    // Security: sprintf to fixed buffer
    char msg[64];
    sprintf(msg, "Sending %d bytes", len);

    return send(data);
}

Connection* create_connection(const Config& config) {
    auto* conn = new Connection(config.host.c_str(), config.port);
    if (g_default_connection == nullptr) {
        g_default_connection = conn;
    }
    return conn;
}

} // namespace demo
