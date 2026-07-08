# Overlay port for mcp.cpp. Use with:
#   vcpkg install mcp-cpp --overlay-ports=<repo>/ports
# Before publishing to a registry: tag a release (e.g. v0.1.0), set REF to
# the tag, and fill SHA512 (vcpkg will print the expected hash on mismatch).

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO oruccakir/mcp.cpp
    REF main
    SHA512 0
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMCP_BUILD_TESTS=OFF
        -DMCP_BUILD_EXAMPLES=OFF
        -DMCP_INSTALL=ON
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME mcp CONFIG_PATH lib/cmake/mcp)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
