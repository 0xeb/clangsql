cmake_minimum_required(VERSION 3.20)
project(clangsql VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output directories (only set if building standalone)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()

# ============================================================================
# libxsql dependency (same pattern as idasql/pdbsql)
# ============================================================================

if(TARGET xsql::xsql)
    # Part of monorepo - already available via add_subdirectory
    message(STATUS "clangsql: Using xsql::xsql from parent project")
else()
    # Standalone build - try find_package first, then fallback to sibling directory
    find_package(xsql QUIET)
    if(xsql_FOUND)
        message(STATUS "clangsql: Using xsql::xsql via find_package")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libxsql/CMakeLists.txt")
        # Monorepo: sibling directory
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libxsql ${CMAKE_CURRENT_BINARY_DIR}/libxsql)
        message(STATUS "clangsql: Added libxsql from sibling directory")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libxsql/CMakeLists.txt")
        # Standalone: submodule in external/
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/libxsql ${CMAKE_CURRENT_BINARY_DIR}/libxsql)
        message(STATUS "clangsql: Added libxsql from external/libxsql submodule")
    else()
        message(FATAL_ERROR "libxsql not found. Either:\n"
            "  1. Build as part of xsql monorepo\n"
            "  2. Set xsql_DIR to the libxsql build directory\n"
            "  3. Install libxsql and ensure it's in CMAKE_PREFIX_PATH\n"
            "  4. Initialize submodule: git submodule update --init external/libxsql")
    endif()
endif()

# ============================================================================
# libclang dependency
# ============================================================================

# Try vcpkg-installed Clang first
find_package(Clang CONFIG QUIET)

if(Clang_FOUND)
    message(STATUS "clangsql: Found Clang via find_package (vcpkg)")
    set(LIBCLANG_LIBRARY libclang)
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

# nlohmann/json is provided by libxsql (xsql::json)

# ============================================================================
# AI Agent Support (optional, requires libagents)
# ============================================================================

option(CLANGSQL_WITH_AI_AGENT "Enable AI agent support" ON)

if(CLANGSQL_WITH_AI_AGENT)
    if(TARGET libagents)
        # Already available from parent
        message(STATUS "clangsql: Using libagents from parent project")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libagents/CMakeLists.txt")
        # Standalone: submodule in external/
        set(LIBAGENTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(LIBAGENTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LIBAGENTS_BUILD_CLI OFF CACHE BOOL "" FORCE)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/libagents ${CMAKE_CURRENT_BINARY_DIR}/libagents)
        message(STATUS "clangsql: Added libagents from external/libagents submodule")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../idasql/external/libagents/CMakeLists.txt")
        # Monorepo fallback: idasql sibling
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../idasql/external/libagents ${CMAKE_CURRENT_BINARY_DIR}/libagents EXCLUDE_FROM_ALL)
        message(STATUS "clangsql: Found libagents from idasql/external/libagents")
    else()
        message(FATAL_ERROR "libagents not found. Either:\n"
            "  1. Initialize submodule: git submodule update --init external/libagents\n"
            "  2. Build as part of xsql monorepo with idasql")
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

# Tests are in the private xsql monorepo (tests/clangsql/)

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
