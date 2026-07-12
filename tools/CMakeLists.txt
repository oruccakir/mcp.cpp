# Standalone bring-up tools (no GTest, no network beyond loopback).
add_executable(pal_selftest pal_selftest.cpp)
target_link_libraries(pal_selftest PRIVATE mcp::core mcp::transport mcp::server mcp::client)
target_include_directories(pal_selftest PRIVATE ${PROJECT_SOURCE_DIR})
mcp_set_warnings(pal_selftest)

# Also runs on host CI so the selftest itself cannot rot.
if(MCP_BUILD_TESTS AND MCP_BUILD_SERVER AND MCP_BUILD_CLIENT)
    add_test(NAME pal_selftest COMMAND pal_selftest)
endif()
