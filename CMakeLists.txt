#
# Copyright (c) 2024-2026 Elias Bachaalany
# SPDX-License-Identifier: LicenseRef-Human-Origin-Source-1.0
#
# This file is licensed under the Human-Origin Source License v1.0.
# See LICENSE.
#
cmake_minimum_required(VERSION 3.20)
project(clangsql VERSION 0.0.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Windows VERSIONINFO wiring (version is from project() above)
set(TOOL_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(TOOL_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(TOOL_VERSION_PATCH ${PROJECT_VERSION_PATCH})
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/versioninfo.cmake)

# Output directories (only set if building standalone)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()

# ============================================================================
# HTTP Server Support (optional, uses cpp-httplib via libxsql thinclient)
# ============================================================================

option(CLANGSQL_WITH_HTTP "Build with HTTP REST server support" ON)

if(CLANGSQL_WITH_HTTP)
    message(STATUS "clangsql: Building with HTTP server support")
    set(XSQL_WITH_THINCLIENT ON CACHE BOOL "" FORCE)
endif()

# ============================================================================
# libxsql dependency
# ============================================================================

if(TARGET xsql::xsql)
    message(STATUS "clangsql: Using xsql::xsql from parent project")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libxsql/CMakeLists.txt")
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libxsql ${CMAKE_CURRENT_BINARY_DIR}/libxsql)
    message(STATUS "clangsql: Added libxsql from sibling directory")
else()
    message(STATUS "clangsql: Fetching libxsql from GitHub...")
    include(FetchContent)
    FetchContent_Declare(libxsql
        GIT_REPOSITORY https://github.com/0xeb/libxsql.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(libxsql)
endif()

# ============================================================================
# libclang dependency
# ============================================================================

# Try vcpkg-installed Clang first
find_package(Clang CONFIG QUIET)

if(Clang_FOUND)
    message(STATUS "clangsql: Found Clang via find_package")
    set(LIBCLANG_LIBRARY libclang)
    # Extract include directory from the imported target
    get_target_property(_clang_inc libclang INTERFACE_INCLUDE_DIRECTORIES)
    if(_clang_inc)
        set(LIBCLANG_INCLUDE_DIR "${_clang_inc}")
    endif()
else()
    # Fallback: find libclang manually
    find_library(LIBCLANG_LIBRARY NAMES clang libclang
        HINTS
            "$ENV{LLVM_DIR}/lib"
            "$ENV{LLVM_ROOT}/lib"
            "/usr/lib/llvm-18/lib"
            "/usr/lib/llvm-17/lib"
            "/usr/lib/llvm-16/lib"
            "/opt/homebrew/opt/llvm/lib"
            "C:/Program Files/LLVM/lib"
    )

    find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
        HINTS
            "$ENV{LLVM_DIR}/include"
            "$ENV{LLVM_ROOT}/include"
            "/usr/lib/llvm-18/include"
            "/usr/lib/llvm-17/include"
            "/usr/lib/llvm-16/include"
            "/opt/homebrew/opt/llvm/include"
            "C:/Program Files/LLVM/include"
    )

    if(LIBCLANG_LIBRARY AND LIBCLANG_INCLUDE_DIR)
        message(STATUS "clangsql: Found libclang: ${LIBCLANG_LIBRARY}")
        message(STATUS "clangsql: Found libclang headers: ${LIBCLANG_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "libclang not found. Either:\n"
            "  1. Install via vcpkg: vcpkg install llvm[clang]\n"
            "  2. Install system LLVM (apt install libclang-dev / brew install llvm)\n"
            "  3. Set LLVM_DIR or LLVM_ROOT environment variable")
    endif()
endif()

# Fallback: find clang-c/Index.h if not yet located (e.g. find_package succeeded
# but the imported target didn't carry INTERFACE_INCLUDE_DIRECTORIES)
if(NOT LIBCLANG_INCLUDE_DIR)
    find_path(LIBCLANG_INCLUDE_DIR clang-c/Index.h
        HINTS
            "$ENV{LLVM_DIR}/include"
            "$ENV{LLVM_ROOT}/include"
            "/usr/lib/llvm-18/include"
            "/usr/lib/llvm-17/include"
            "/opt/homebrew/opt/llvm/include"
            "C:/Program Files/LLVM/include"
    )
    if(LIBCLANG_INCLUDE_DIR)
        message(STATUS "clangsql: Found libclang headers (fallback): ${LIBCLANG_INCLUDE_DIR}")
    endif()
endif()

# nlohmann/json is provided by libxsql (xsql::json)

# ============================================================================
# MCP Support (optional, uses fastmcpp)
# ============================================================================

option(CLANGSQL_WITH_MCP "Build with MCP server support (fastmcpp)" ON)

if(CLANGSQL_WITH_MCP)
    message(STATUS "clangsql: Building with MCP support via fastmcpp")

    # Reuse an existing fastmcpp target when available in a parent build.
    if(NOT TARGET fastmcpp_core)
        # Keep the dependency surface minimal for clangsql.
        set(FASTMCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(FASTMCPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(FASTMCPP_ENABLE_POST_STREAMING OFF CACHE BOOL "" FORCE)
        set(FASTMCPP_ENABLE_SAMPLING_HTTP_HANDLERS OFF CACHE BOOL "" FORCE)
        set(FASTMCPP_FETCH_CURL OFF CACHE BOOL "" FORCE)

        message(STATUS "clangsql: Fetching fastmcpp from GitHub...")
        include(FetchContent)
        FetchContent_Declare(fastmcpp
            GIT_REPOSITORY https://github.com/0xeb/fastmcpp.git
            GIT_TAG        main
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(fastmcpp)
    endif()

    if(MSVC AND TARGET fastmcpp_core)
        target_compile_options(fastmcpp_core PRIVATE /MP)
    endif()
endif()

# ============================================================================
# Include directories
# ============================================================================

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/include)
if(LIBCLANG_INCLUDE_DIR)
    include_directories(${LIBCLANG_INCLUDE_DIR})
endif()

# ============================================================================
# Subdirectories
# ============================================================================

add_subdirectory(src)


# ============================================================================
# Install & Export (for find_package support)
# ============================================================================

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

# Install headers
install(DIRECTORY src/include/clangsql
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Configure and install package config files
configure_package_config_file(
    cmake/clangsqlConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/clangsqlConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/clangsql
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/clangsqlConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/clangsqlConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/clangsqlConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/clangsql
)
