cmake_minimum_required(VERSION 3.20)
project(demo VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# All sources
set(SOURCES
    main.cpp
    handlers.cpp
    network.cpp
    utils.cpp
)

# Create executable
add_executable(demo ${SOURCES})
