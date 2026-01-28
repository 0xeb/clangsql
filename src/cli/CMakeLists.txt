# clangsql CLI tool

# Conditionally add agent sources
set(CLI_SOURCES main.cpp)
if(CLANGSQL_WITH_AI_AGENT)
    list(APPEND CLI_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/ai_agent.cpp
    )

    # Generate embedded prompt header
    set(PROMPT_MD "${CMAKE_CURRENT_SOURCE_DIR}/../../prompts/clangsql_agent.md")
    set(PROMPT_HPP "${CMAKE_CURRENT_BINARY_DIR}/clangsql_agent_prompt.hpp")

    add_custom_command(
        OUTPUT ${PROMPT_HPP}
        COMMAND ${CMAKE_COMMAND} -E env python3 ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/embed_prompt.py ${PROMPT_MD} ${PROMPT_HPP}
        DEPENDS ${PROMPT_MD} ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/embed_prompt.py
        COMMENT "Embedding clangsql_agent.md into clangsql_agent_prompt.hpp"
    )

    # Add generated header to sources
    list(APPEND CLI_SOURCES ${PROMPT_HPP})
endif()

add_executable(clangsql_cli ${CLI_SOURCES})
set_target_properties(clangsql_cli PROPERTIES OUTPUT_NAME clangsql)

# Include generated files directory
target_include_directories(clangsql_cli PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(clangsql_cli PRIVATE
    clangsql::clangsql
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

# Add AI agent support
if(CLANGSQL_WITH_AI_AGENT)
    target_compile_definitions(clangsql_cli PRIVATE CLANGSQL_HAS_AI_AGENT)
    target_link_libraries(clangsql_cli PRIVATE libagents::libagents)
    target_include_directories(clangsql_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
endif()

# Add HTTP server support
if(CLANGSQL_WITH_HTTP)
    target_compile_definitions(clangsql_cli PRIVATE CLANGSQL_HAS_HTTP)
endif()

# Install CLI
install(TARGETS clangsql_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
