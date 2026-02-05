#include "types.h"
#include "network.h"
#include "handlers.h"
#include "utils.h"
#include <cstdio>

using namespace demo;

// More global state
static int g_init_count = 0;
char g_app_name[256] = "demo_app";

void init_handlers() {
    auto& registry = HandlerRegistry::instance();
    registry.register_handler(new JsonHandler());
    registry.register_handler(new BinaryHandler());
    registry.register_handler(new XmlHandler());
    g_init_count++;
}

// Security: user input directly in format string
void greet_user(const char* username) {
    log_message(username);  // Format string vulnerability!
}

// Callback for async processing
void on_complete(int status, void* data) {
    printf("Complete: %d\n", status);
    if (data) {
        // Security: casting void* without validation
        int* value = (int*)data;
        printf("Value: %d\n", *value);
    }
}

int main(int argc, char* argv[]) {
    // Initialize
    init_handlers();

    // Create connection
    Config config;
    config.host = "localhost";
    config.port = 8080;
    config.timeout_ms = 5000;
    config.use_ssl = false;

    Connection* conn = create_connection(config);
    if (!conn->connect()) {
        return 1;
    }

    // Process some data
    const char* data = "{\"test\": true}";
    auto& registry = HandlerRegistry::instance();

    Handler* json = registry.find("json");
    if (json) {
        json->process(data, strlen(data));
    }

    // Use the network
    conn->send(data);

    // Risky: user input as format string
    if (argc > 1) {
        greet_user(argv[1]);
    }

    // Process buffer with many params
    char input[] = "hello";
    char output[256];
    size_t output_len = sizeof(output);
    int result_val = 42;

    process_buffer(input, strlen(input),
                  output, &output_len,
                  0, 1,
                  on_complete, &result_val);

    // Cleanup
    delete conn;

    return 0;
}
