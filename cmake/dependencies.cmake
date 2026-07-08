include(FetchContent)

# nlohmann/json 3.11+ (FR-BUILD-004). Prefer a system/package-manager
# install; otherwise use the vendored single header (third_party/nlohmann,
# v3.11.3, MIT) — no network access at configure time.
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    add_library(nlohmann_json INTERFACE)
    add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
    target_include_directories(nlohmann_json INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party>
        $<INSTALL_INTERFACE:include>)
    message(STATUS "nlohmann/json: using vendored third_party/nlohmann (3.11.3)")
endif()

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

find_package(Threads REQUIRED)
