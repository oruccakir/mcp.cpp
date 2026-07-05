# Helper binary spawned by the integration tests.
add_executable(echo_stdio tools/echo_stdio.cpp)
target_link_libraries(echo_stdio PRIVATE mcp::core mcp::transport)
mcp_set_warnings(echo_stdio)

add_executable(mcp_tests
    test_error.cpp
    test_message.cpp
    test_router.cpp
    test_session.cpp
    test_stdio.cpp
    test_integration.cpp
)
target_link_libraries(mcp_tests PRIVATE mcp::core mcp::transport GTest::gtest_main)
target_compile_definitions(mcp_tests PRIVATE
    ECHO_STDIO_PATH="$<TARGET_FILE:echo_stdio>")
add_dependencies(mcp_tests echo_stdio)
mcp_set_warnings(mcp_tests)

include(GoogleTest)
gtest_discover_tests(mcp_tests PROPERTIES TIMEOUT 60)
