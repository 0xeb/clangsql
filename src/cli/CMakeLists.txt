# clangsql CLI tool

set(CLI_SOURCES main.cpp)

add_executable(clangsql_cli ${CLI_SOURCES})
set_target_properties(clangsql_cli PROPERTIES OUTPUT_NAME clangsql)

# Windows VS_VERSION_INFO resource
xsql_add_versioninfo(clangsql_cli
    FILETYPE          VFT_APP
    DESCRIPTION       "clangsql command-line interface for Clang AST"
    ORIGINAL_FILENAME "clangsql.exe")

# Include generated files directory
target_include_directories(clangsql_cli PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(clangsql_cli PRIVATE
    clangsql::clangsql
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

# Add MCP server support
if(CLANGSQL_WITH_MCP)
    target_compile_definitions(clangsql_cli PRIVATE CLANGSQL_HAS_MCP)
    target_sources(clangsql_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/mcp_server.cpp
    )
    target_link_libraries(clangsql_cli PRIVATE fastmcpp_core)
    target_include_directories(clangsql_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
endif()

# Add HTTP server support
if(CLANGSQL_WITH_HTTP)
    target_compile_definitions(clangsql_cli PRIVATE CLANGSQL_HAS_HTTP)
    target_sources(clangsql_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/http_server.cpp
    )
    # Ensure common headers are available (may already be added by agent support)
    target_include_directories(clangsql_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
endif()

# Install CLI
install(TARGETS clangsql_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
