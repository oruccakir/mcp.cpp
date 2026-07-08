# Standalone consumer of an *installed* mcp package — not part of the main
# build. CI installs the SDK into a prefix and configures this project
# against it (CMAKE_PREFIX_PATH) to prove the export surface works.
cmake_minimum_required(VERSION 3.20)
project(mcp-consumer CXX)

find_package(mcp CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE mcp::server mcp::transport)
set_target_properties(consumer PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
