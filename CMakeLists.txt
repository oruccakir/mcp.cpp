cmake_minimum_required(VERSION 3.20)

project(mcpcpp
    VERSION 0.1.0
    DESCRIPTION "MCP (Model Context Protocol) C++ SDK"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MCP_IS_TOP_LEVEL ON)
else()
    set(MCP_IS_TOP_LEVEL OFF)
endif()

option(MCP_BUILD_TESTS "Build the mcp.cpp test suite" ${MCP_IS_TOP_LEVEL})
option(MCP_BUILD_EXAMPLES "Build the mcp.cpp example servers" ${MCP_IS_TOP_LEVEL})
option(MCP_USE_EXCEPTIONS "Enable exception-based error reporting" ON)
option(MCP_WERROR "Treat compiler warnings as errors" OFF)
option(MCP_ENABLE_LOGGING
    "Compile in MCP_LOG diagnostic logging (stderr; runtime level via MCP_LOG env)"
    OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(warnings)
include(dependencies)

add_subdirectory(src)

if(MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
