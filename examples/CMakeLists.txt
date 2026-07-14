foreach(example echo_server calculator_server echo_server_http)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE mcp::core mcp::transport mcp::server)
    mcp_set_warnings(${example})
endforeach()

# System-monitor server: real kernel introspection on VxWorks, simulated data
# elsewhere so the MCP surface still builds and runs on host CI.
set(MONITOR_BACKEND vxworks_monitor/monitor_backend_sim.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "VxWorks")
    set(MONITOR_BACKEND vxworks_monitor/monitor_backend_vxworks.cpp)
endif()
add_executable(vxworks_monitor_server
    vxworks_monitor/vxworks_monitor_server.cpp
    ${MONITOR_BACKEND})
target_link_libraries(vxworks_monitor_server
    PRIVATE mcp::core mcp::transport mcp::server)
mcp_set_warnings(vxworks_monitor_server)
