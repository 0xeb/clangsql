cmake_minimum_required(VERSION 3.16)
project(sample_project VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generate compile_commands.json for clangsql
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Define some macros for testing
add_compile_definitions(
    PROJECT_VERSION="${PROJECT_VERSION}"
    ENABLE_LOGGING=1
)

# Library
add_library(sample_lib STATIC
    src/math_utils.cpp
    src/string_utils.cpp
)

# Executable
add_executable(sample_app
    src/main.cpp
)

target_link_libraries(sample_app PRIVATE sample_lib)

# Optional: Install rules
install(TARGETS sample_app sample_lib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
