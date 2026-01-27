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
