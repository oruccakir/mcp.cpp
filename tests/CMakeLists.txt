# Helper binaries spawned by the integration tests.
add_executable(echo_stdio tools/echo_stdio.cpp)
target_link_libraries(echo_stdio PRIVATE mcp::core mcp::transport)
mcp_set_warnings(echo_stdio)

add_executable(toolbox_stdio tools/toolbox_stdio.cpp)
target_link_libraries(toolbox_stdio PRIVATE
    mcp::core mcp::transport mcp::server mcp::client)
mcp_set_warnings(toolbox_stdio)

add_executable(prober_stdio tools/prober_stdio.cpp)
target_link_libraries(prober_stdio PRIVATE
    mcp::core mcp::transport mcp::server mcp::client)
mcp_set_warnings(prober_stdio)

add_executable(mcp_tests
    test_error.cpp
    test_message.cpp
    test_router.cpp
    test_session.cpp
    test_stdio.cpp
    test_integration.cpp
    test_content.cpp
    test_schema.cpp
    test_tools.cpp
    test_resources.cpp
    test_prompts.cpp
    test_server.cpp
    test_server_integration.cpp
    test_sampling.cpp
    test_roots.cpp
    test_client.cpp
    test_client_integration.cpp
    test_http_codec.cpp
    test_http_transport.cpp
    test_http_session.cpp
    test_http_integration.cpp
)
target_link_libraries(mcp_tests PRIVATE
    mcp::core mcp::transport mcp::server mcp::client GTest::gtest_main)
target_compile_definitions(mcp_tests PRIVATE
    ECHO_STDIO_PATH="$<TARGET_FILE:echo_stdio>"
    TOOLBOX_STDIO_PATH="$<TARGET_FILE:toolbox_stdio>"
    PROBER_STDIO_PATH="$<TARGET_FILE:prober_stdio>"
    ECHO_SERVER_HTTP_PATH="$<TARGET_FILE:echo_server_http>")
add_dependencies(mcp_tests echo_stdio toolbox_stdio prober_stdio echo_server_http)
mcp_set_warnings(mcp_tests)

include(GoogleTest)
gtest_discover_tests(mcp_tests PROPERTIES TIMEOUT 60)
