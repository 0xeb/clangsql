#pragma once
#include "types.h"
#include <memory>
#include <vector>

namespace demo {

// Concrete handler - JSON
class JsonHandler : public Handler {
public:
    bool process(const char* data, size_t len) override;
    const char* name() const override { return "json"; }

    // Security: exposes internal state
    char* get_buffer() { return buffer_; }

private:
    char buffer_[4096];
    int parse_count_ = 0;
};

// Concrete handler - Binary
class BinaryHandler : public Handler {
public:
    bool process(const char* data, size_t len) override;
    const char* name() const override { return "binary"; }

    void set_callback(void (*cb)(void*)) { callback_ = cb; }

private:
    void (*callback_)(void*) = nullptr;  // Function pointer
    std::vector<uint8_t> data_;
};

// Concrete handler - XML (deprecated patterns)
class XmlHandler : public Handler {
public:
    XmlHandler();
    ~XmlHandler();

    bool process(const char* data, size_t len) override;
    const char* name() const override { return "xml"; }

    // Security: raw pointer getters
    char* get_root_element() { return root_; }
    char* get_last_error() { return error_; }

private:
    char* root_;   // Owning pointer
    char* error_;  // Owning pointer
};

// Handler registry (singleton-ish)
class HandlerRegistry {
public:
    static HandlerRegistry& instance();

    void register_handler(Handler* handler);  // Takes ownership? Unclear
    Handler* find(const char* name);
    void process_all(const char* data, size_t len);

private:
    HandlerRegistry() = default;
    std::vector<Handler*> handlers_;  // Ownership unclear
};

} // namespace demo
