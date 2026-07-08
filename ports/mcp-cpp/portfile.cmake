# Overlay port for mcp.cpp. Use with:
#   vcpkg install mcp-cpp --overlay-ports=<repo>/ports
# On each release: bump vcpkg.json's version, point REF at the new tag, and
# refresh SHA512 (sha512sum of the tag's source tarball).

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO oruccakir/mcp.cpp
    REF v0.1.0
    SHA512 88fa342b7a7842d40bb50b661aa3d150d8202b917a2dfb0dbdc0d0cef9e013778fefaebf72ea5177160a0389d6a6a71b023b862261adb8d42f81f44ff995298a
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
