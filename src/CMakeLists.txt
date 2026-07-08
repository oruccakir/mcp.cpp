# mcp-core: JSON-RPC engine, router, session, protocol types (FR-BUILD-001)
add_library(mcp-core STATIC
    jsonrpc/message.cpp
    core/router.cpp
    core/session.cpp
    core/content.cpp
    core/schema.cpp
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

# mcp-transport: stdio + Streamable HTTP transports (POSIX)
add_library(mcp-transport STATIC
    transport/line_transport.cpp
    transport/line_io.cpp
    transport/stdio_transport.cpp
    transport/stdio_client_transport.cpp
    transport/http/socket_util.cpp
    transport/http/http_codec.cpp
    transport/http/http_endpoint.cpp
    transport/http/sse_channel.cpp
    transport/http_server_transport.cpp
    transport/http_client_transport.cpp
    transport/http_session_server.cpp
)
add_library(mcp::transport ALIAS mcp-transport)
target_link_libraries(mcp-transport PUBLIC mcp-core)
mcp_set_warnings(mcp-transport)

# mcp-server: tools, resources, prompts, logging, completions, Server facade
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

# mcp-client: sampling, roots, elicitation, Client facade.
# Links mcp-server for the shared feature types (Tool, Resource, Prompt)
# used by the typed client wrappers.
add_library(mcp-client STATIC
    client/sampling.cpp
    client/roots_provider.cpp
    client/client.cpp
)
add_library(mcp::client ALIAS mcp-client)
target_link_libraries(mcp-client PUBLIC mcp-core mcp-server)
mcp_set_warnings(mcp-client)
