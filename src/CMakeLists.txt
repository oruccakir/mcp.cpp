# Platform Abstraction Layer backend selection (SRS §3.1, FR-EMB-002).
# Transports only ever call the pal:: API from src/platform/pal.hpp; adding
# an OS means adding a backend directory here, not touching transport code.
if(CMAKE_SYSTEM_NAME STREQUAL "VxWorks")
    # VxWorks 7 RTPs: POSIX io/net, but no fork/exec — the process module
    # (and with it the stdio client transport) is compiled out.
    set(MCP_PAL_BACKEND "vxworks")
    set(MCP_PAL_SOURCES
        platform/vxworks/io.cpp
        platform/vxworks/net.cpp
    )
    set(MCP_PAL_HAS_PROCESS OFF)
elseif(WIN32)
    set(MCP_PAL_BACKEND "win32")
    set(MCP_PAL_SOURCES
        platform/win32/io.cpp
        platform/win32/net.cpp
        platform/win32/process.cpp
    )
    set(MCP_PAL_HAS_PROCESS ON)
elseif(UNIX)
    set(MCP_PAL_BACKEND "posix")
    set(MCP_PAL_SOURCES
        platform/posix/io.cpp
        platform/posix/net.cpp
        platform/posix/process.cpp
    )
    set(MCP_PAL_HAS_PROCESS ON)
else()
    message(FATAL_ERROR "No PAL backend for this platform")
endif()

# Configure-time banner: which platform this build targets and how the
# platform-dependent pieces resolved (CMAKE_SYSTEM_NAME comes from the host
# OS on native builds, or from the toolchain file when cross-compiling).
if(CMAKE_CROSSCOMPILING)
    set(MCP_BUILD_KIND "cross-compiling on ${CMAKE_HOST_SYSTEM_NAME}")
else()
    set(MCP_BUILD_KIND "native")
endif()
message(STATUS "----------------------------------------------")
message(STATUS "mcp.cpp ${PROJECT_VERSION}")
message(STATUS "  target platform : ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR}), ${MCP_BUILD_KIND}")
message(STATUS "  compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  PAL backend     : ${MCP_PAL_BACKEND}")
message(STATUS "  process spawn   : ${MCP_PAL_HAS_PROCESS}")
message(STATUS "  server / client : ${MCP_BUILD_SERVER} / ${MCP_BUILD_CLIENT}")
message(STATUS "  MCP_LOG         : ${MCP_ENABLE_LOGGING}")
message(STATUS "----------------------------------------------")

# mcp-core: JSON-RPC engine, router, session, protocol types (FR-BUILD-001)
add_library(mcp-core STATIC
    jsonrpc/message.cpp
    core/router.cpp
    core/session.cpp
    core/content.cpp
    core/schema.cpp
    core/log.cpp
)
add_library(mcp::core ALIAS mcp-core)
target_include_directories(mcp-core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(mcp-core PUBLIC
    nlohmann_json::nlohmann_json
    Threads::Threads
)
target_compile_features(mcp-core PUBLIC cxx_std_17)
if(MCP_ENABLE_LOGGING)
    target_compile_definitions(mcp-core PUBLIC MCP_LOG_ENABLED)
endif()
if(MCP_PAL_HAS_PROCESS)
    target_compile_definitions(mcp-core PUBLIC MCP_PAL_HAS_PROCESS)
endif()
mcp_set_warnings(mcp-core)

# mcp-transport: stdio + Streamable HTTP transports over the PAL
add_library(mcp-transport STATIC
    ${MCP_PAL_SOURCES}
    transport/line_transport.cpp
    transport/line_io.cpp
    transport/stdio_transport.cpp
    transport/http/http_codec.cpp
    transport/http/http_endpoint.cpp
    transport/http/sse_channel.cpp
    transport/http_server_transport.cpp
    transport/http_client_transport.cpp
    transport/http_session_server.cpp
)
if(MCP_PAL_HAS_PROCESS)
    target_sources(mcp-transport PRIVATE transport/stdio_client_transport.cpp)
endif()
add_library(mcp::transport ALIAS mcp-transport)
target_include_directories(mcp-transport PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mcp-transport PUBLIC mcp-core)
if(WIN32)
    target_link_libraries(mcp-transport PUBLIC ws2_32)
endif()
mcp_set_warnings(mcp-transport)

# mcp-server: tools, resources, prompts, logging, completions, Server facade
if(MCP_BUILD_SERVER)
add_library(mcp-server STATIC
    server/tool_registry.cpp
    server/resource_provider.cpp
    server/prompt_provider.cpp
    server/logging.cpp
    server/uri_template.cpp
    server/server.cpp
)
add_library(mcp::server ALIAS mcp-server)
target_link_libraries(mcp-server PUBLIC mcp-core)
mcp_set_warnings(mcp-server)
endif()

# mcp-client: sampling, roots, elicitation, Client facade.
# Links mcp-server for the shared feature types (Tool, Resource, Prompt)
# used by the typed client wrappers.
if(MCP_BUILD_CLIENT)
add_library(mcp-client STATIC
    client/sampling.cpp
    client/roots_provider.cpp
    client/client.cpp
)
add_library(mcp::client ALIAS mcp-client)
target_link_libraries(mcp-client PUBLIC mcp-core mcp-server)
mcp_set_warnings(mcp-client)
endif()

# --- Install (FR-BUILD-003): one export set, namespace mcp:: ---------------
if(MCP_INSTALL)
    include(GNUInstallDirs)
    # Installed names match the in-tree aliases (mcp::core, mcp::server, ...).
    set_target_properties(mcp-core PROPERTIES EXPORT_NAME core)
    set_target_properties(mcp-transport PROPERTIES EXPORT_NAME transport)
    set_target_properties(nlohmann_json PROPERTIES EXPORT_NAME vendored-json)
    if(MCP_BUILD_SERVER)
        set_target_properties(mcp-server PROPERTIES EXPORT_NAME server)
    endif()
    if(MCP_BUILD_CLIENT)
        set_target_properties(mcp-client PROPERTIES EXPORT_NAME client)
    endif()
    # The vendored json INTERFACE target must join the export set so the
    # libraries' recorded link dependency resolves as mcp::nlohmann_json.
    install(TARGETS mcp-core mcp-transport nlohmann_json
        EXPORT mcp-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    if(MCP_BUILD_SERVER)
        install(TARGETS mcp-server EXPORT mcp-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    if(MCP_BUILD_CLIENT)
        install(TARGETS mcp-client EXPORT mcp-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
endif()
