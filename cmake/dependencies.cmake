include(FetchContent)

# nlohmann/json 3.11.3, always vendored (third_party/, MIT): deterministic
# builds and a self-contained install export — consumers of an installed
# mcp package need no json find_dependency (FR-BUILD-004). Distro packagers
# may patch this to a system package. Installed under include/mcp/vendor so
# it cannot clash with a real nlohmann install in the same prefix.
add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party>
    $<INSTALL_INTERFACE:include/mcp/vendor>)

if(MCP_BUILD_TESTS)
    find_package(GTest 1.12 QUIET)
    if(NOT GTest_FOUND)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
            URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()

# Host threading via pthreads. On VxWorks the kernel provides taskLib/semLib
# built in (no pthread lib — find_package(Threads) fails there with
# CMAKE_HAVE_LIBC_PTHREAD not found), and mcp::sys uses native tasks instead.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "VxWorks")
    find_package(Threads REQUIRED)
endif()
