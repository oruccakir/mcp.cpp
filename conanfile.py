from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout


class McpCppConan(ConanFile):
    name = "mcp-cpp"
    version = "0.1.0"
    description = ("C++ SDK for the Model Context Protocol (MCP): servers "
                   "and clients over stdio and Streamable HTTP.")
    url = "https://github.com/oruccakir/mcp.cpp"
    # TODO: add a top-level LICENSE file to the repo, then set it here.
    license = "LicenseRef-pending"
    settings = "os", "compiler", "build_type", "arch"
    exports_sources = ("CMakeLists.txt", "CMakePresets.json", "cmake/*",
                       "include/*", "src/*", "third_party/*")
    options = {"build_server": [True, False], "build_client": [True, False]}
    default_options = {"build_server": True, "build_client": True}

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["MCP_BUILD_TESTS"] = False
        tc.variables["MCP_BUILD_EXAMPLES"] = False
        tc.variables["MCP_BUILD_SERVER"] = self.options.build_server
        tc.variables["MCP_BUILD_CLIENT"] = self.options.build_client
        tc.variables["MCP_INSTALL"] = True
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "mcp")

        core = self.cpp_info.components["core"]
        core.libs = ["mcp-core"]
        core.set_property("cmake_target_name", "mcp::core")
        core.includedirs = ["include", "include/mcp/vendor"]
        if self.settings.os in ("Linux", "FreeBSD"):
            core.system_libs = ["pthread"]

        transport = self.cpp_info.components["transport"]
        transport.libs = ["mcp-transport"]
        transport.set_property("cmake_target_name", "mcp::transport")
        transport.requires = ["core"]
        if self.settings.os == "Windows":
            transport.system_libs = ["ws2_32"]

        if self.options.build_server:
            server = self.cpp_info.components["server"]
            server.libs = ["mcp-server"]
            server.set_property("cmake_target_name", "mcp::server")
            server.requires = ["core"]

        if self.options.build_client:
            client = self.cpp_info.components["client"]
            client.libs = ["mcp-client"]
            client.set_property("cmake_target_name", "mcp::client")
            client.requires = ["core", "server"]
