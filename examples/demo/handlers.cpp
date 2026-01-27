#include "handlers.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

namespace demo {

// JsonHandler implementation
bool JsonHandler::process(const char* data, size_t len) {
    // Security: potential buffer overflow
    memcpy(buffer_, data, len);
    buffer_[len] = '\0';
    parse_count_++;
    return true;
}

// BinaryHandler implementation
bool BinaryHandler::process(const char* data, size_t len) {
    data_.assign(data, data + len);
    if (callback_) {
        callback_(data_.data());
    }
    return true;
}

// XmlHandler implementation
XmlHandler::XmlHandler() {
    root_ = (char*)malloc(1024);
    error_ = (char*)malloc(256);
    strcpy(root_, "<root/>");
    error_[0] = '\0';
}

XmlHandler::~XmlHandler() {
    free(root_);
    free(error_);
}

bool XmlHandler::process(const char* data, size_t len) {
    // Security: strcpy without size check
    if (len < 1024) {
        strcpy(root_, data);
        return true;
    }
    sprintf(error_, "Data too large: %zu bytes", len);
    return false;
}

// HandlerRegistry implementation
HandlerRegistry& HandlerRegistry::instance() {
    static HandlerRegistry registry;
    return registry;
}

void HandlerRegistry::register_handler(Handler* handler) {
    handlers_.push_back(handler);
}

Handler* HandlerRegistry::find(const char* name) {
    for (auto* h : handlers_) {
        if (strcmp(h->name(), name) == 0) {
            return h;
        }
    }
    return nullptr;
}

void HandlerRegistry::process_all(const char* data, size_t len) {
    for (auto* h : handlers_) {
        h->process(data, len);
    }
}

} // namespace demo
