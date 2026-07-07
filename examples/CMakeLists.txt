foreach(example echo_server calculator_server)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE mcp::core mcp::transport mcp::server)
    mcp_set_warnings(${example})
endforeach()

add_executable(echo_server_http echo_server_http.cpp)
target_link_libraries(echo_server_http PRIVATE
    mcp::core mcp::transport mcp::server mcp::client)
mcp_set_warnings(echo_server_http)
