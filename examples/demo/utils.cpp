#include "utils.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

namespace demo {

// Security: format string passed directly to printf
void log_message(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

// Security: returns static buffer (not thread-safe, dangling risk)
char* format_string(const char* fmt, ...) {
    static char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsprintf(buffer, fmt, args);
    va_end(args);
    return buffer;
}

// Security: no bounds checking
void copy_data(char* dest, const char* src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = '\0';
}

int process_buffer(void* input, size_t input_len,
                  void* output, size_t* output_len,
                  int flags, int mode,
                  void (*callback)(int, void*),
                  void* user_data) {
    (void)flags;
    (void)mode;

    // Just copy input to output
    if (*output_len < input_len) {
        return -1;
    }

    memcpy(output, input, input_len);
    *output_len = input_len;

    if (callback) {
        callback(0, user_data);
    }

    return 0;
}

size_t calculate_hash(const char* data, size_t len) {
    size_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)data[i];
    }
    return hash;
}

int string_to_int(const char* str, int* out_value) {
    if (!str || !out_value) return -1;
    *out_value = atoi(str);
    return 0;
}

// Security: caller must free, but not documented
char* int_to_string(int value) {
    char* buffer = (char*)malloc(32);
    sprintf(buffer, "%d", value);
    return buffer;
}

void* safe_malloc(size_t size) {
    void* ptr = malloc(size);
    if (!ptr && size > 0) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return ptr;
}

void* safe_realloc(void* ptr, size_t size) {
    void* new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    return new_ptr;
}

} // namespace demo
