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
option(MCP_BUILD_SERVER "Build the server SDK (mcp::server)" ON)
option(MCP_BUILD_CLIENT "Build the client SDK (mcp::client)" ON)
option(MCP_USE_EXCEPTIONS "Enable exception-based error reporting" ON)
option(MCP_WERROR "Treat compiler warnings as errors" OFF)
option(MCP_ENABLE_LOGGING
    "Compile in MCP_LOG diagnostic logging (stderr; runtime level via MCP_LOG env)"
    OFF)
option(MCP_INSTALL "Generate install rules and package config" ${MCP_IS_TOP_LEVEL})

# The client SDK links the server feature types (FR-BUILD-002).
if(MCP_BUILD_CLIENT AND NOT MCP_BUILD_SERVER)
    message(STATUS "MCP_BUILD_CLIENT requires the server types; enabling MCP_BUILD_SERVER")
    set(MCP_BUILD_SERVER ON)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(warnings)
include(dependencies)

# Generated version header (installed alongside the public headers).
configure_file(include/mcp/version.hpp.in
    "${PROJECT_BINARY_DIR}/generated/mcp/version.hpp" @ONLY)

add_subdirectory(src)

if(MCP_BUILD_TESTS)
    if(MCP_BUILD_SERVER AND MCP_BUILD_CLIENT)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "Tests require MCP_BUILD_SERVER and MCP_BUILD_CLIENT; skipping")
    endif()
endif()

if(MCP_BUILD_EXAMPLES)
    if(MCP_BUILD_SERVER)
        add_subdirectory(examples)
    else()
        message(STATUS "Examples require MCP_BUILD_SERVER; skipping")
    endif()
endif()

# --- Install & package config (FR-BUILD-003) --------------------------------
if(MCP_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(DIRECTORY include/mcp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "*.in" EXCLUDE)
    install(FILES "${PROJECT_BINARY_DIR}/generated/mcp/version.hpp"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mcp)
    # Vendored nlohmann/json, namespaced under mcp/vendor (no clash with a
    # real nlohmann install; matches the target's INSTALL_INTERFACE).
    install(FILES third_party/nlohmann/json.hpp third_party/nlohmann/LICENSE.MIT
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mcp/vendor/nlohmann)

    install(EXPORT mcp-targets
        NAMESPACE mcp::
        FILE mcpTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp)

    configure_package_config_file(cmake/mcpConfig.cmake.in
        "${PROJECT_BINARY_DIR}/mcpConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp)
    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/mcpConfigVersion.cmake"
        COMPATIBILITY SameMajorVersion)
    install(FILES
        "${PROJECT_BINARY_DIR}/mcpConfig.cmake"
        "${PROJECT_BINARY_DIR}/mcpConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp)
endif()
