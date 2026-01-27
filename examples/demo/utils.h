#pragma once
#include <cstddef>

namespace demo {

// Utility functions with various issues

// Security: format string vulnerability potential
void log_message(const char* format, ...);

// Security: returns pointer to static buffer
char* format_string(const char* fmt, ...);

// Security: no size parameter for output
void copy_data(char* dest, const char* src);

// Security: many params, complex signature
int process_buffer(void* input, size_t input_len,
                  void* output, size_t* output_len,
                  int flags, int mode,
                  void (*callback)(int, void*),
                  void* user_data);

// Good: const-correct, clear ownership
size_t calculate_hash(const char* data, size_t len);

// Conversion utilities
int string_to_int(const char* str, int* out_value);
char* int_to_string(int value);  // Who frees this?

// Memory utilities
void* safe_malloc(size_t size);
void* safe_realloc(void* ptr, size_t size);

} // namespace demo
