# clangsql library (compiled library with virtual table implementations)

add_library(clangsql STATIC
    tables.cpp
)
add_library(clangsql::clangsql ALIAS clangsql)

target_include_directories(clangsql PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(clangsql PUBLIC
    xsql::xsql        # Provides SQLite and nlohmann_json
    ${LIBCLANG_LIBRARY}
)

# Require C++17
target_compile_features(clangsql PUBLIC cxx_std_17)

# CLI tool
add_subdirectory(cli)

# Install targets disabled for now - xsql dependency complicates exports
# TODO: Enable when ready for packaging
# install(TARGETS clangsql ...)
