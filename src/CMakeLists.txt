# mcp-core: JSON-RPC engine, router, session, protocol types (FR-BUILD-001)
add_library(mcp-core STATIC
    jsonrpc/message.cpp
    core/router.cpp
    core/session.cpp
)
add_library(mcp::core ALIAS mcp-core)
target_include_directories(mcp-core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(mcp-core PUBLIC
    nlohmann_json::nlohmann_json
    Threads::Threads
)
target_compile_features(mcp-core PUBLIC cxx_std_17)
mcp_set_warnings(mcp-core)

# mcp-transport: stdio transports (POSIX)
add_library(mcp-transport STATIC
    transport/line_transport.cpp
    transport/line_io.cpp
    transport/stdio_transport.cpp
    transport/stdio_client_transport.cpp
)
add_library(mcp::transport ALIAS mcp-transport)
target_link_libraries(mcp-transport PUBLIC mcp-core)
mcp_set_warnings(mcp-transport)
