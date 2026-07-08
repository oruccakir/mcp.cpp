// End-to-end: spawn the echo_stdio server as a real subprocess and run the
// full MCP handshake and request round-trips over pipes (FR-TEST-002).

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include <mcp/core/session.hpp>
#include <mcp/transport/stdio_client_transport.hpp>

#include "../src/platform/pal.hpp"

namespace {

using namespace mcp;

ClientOptions test_client_options() {
    ClientOptions options;
    options.client_info = Implementation{"integration-test", std::nullopt, "1.0"};
    options.initialize_timeout = std::chrono::milliseconds(5000);
    return options;
}

TEST(Integration, FullHandshakeAndEchoRoundTrip) {
    StdioServerParameters parameters;
    parameters.command = ECHO_STDIO_PATH;

    auto transport = std::make_shared<StdioClientTransport>(parameters);

    std::mutex stderr_mutex;
    std::vector<std::string> stderr_lines;
    transport->set_stderr_handler([&](std::string line) {
        std::lock_guard<std::mutex> lock(stderr_mutex);
        stderr_lines.push_back(std::move(line));
    });

    ClientSession session(transport, test_client_options());
    session.connect();

    // FR-CORE-010: full initialize handshake against a real subprocess.
    auto init = session.initialize();
    ASSERT_TRUE(init) << init.error().message;
    EXPECT_EQ(init.value().server_info.name, "echo-stdio");
    EXPECT_EQ(init.value().protocol_version, kProtocolVersion);
    ASSERT_TRUE(init.value().instructions.has_value());
    EXPECT_EQ(session.state(), SessionState::Operating);

    // Tool-style request round-trip.
    Session::RequestOptions options;
    options.timeout = std::chrono::milliseconds(5000);
    auto echo = session.send_request_sync("echo", json{{"message", "hi"}}, options);
    ASSERT_TRUE(echo) << echo.error().message;
    EXPECT_EQ(echo.value().at("echo").at("message"), "hi");

    // Ping health check (FR-CORE-017).
    auto pong = session.send_request_sync("ping", std::nullopt, options);
    ASSERT_TRUE(pong) << pong.error().message;
    EXPECT_EQ(pong.value(), json::object());

    // stderr was captured as informational logging (FR-TRAN-002).
    {
        std::lock_guard<std::mutex> lock(stderr_mutex);
        ASSERT_FALSE(stderr_lines.empty());
        EXPECT_EQ(stderr_lines[0], "echo-stdio started");
    }

    // FR-TRAN-004: clean shutdown; closing stdin lets the child exit.
    session.disconnect();
    auto status = transport->exit_status();
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(pal::exited_normally(*status));
    EXPECT_EQ(pal::exit_code(*status), 0);
}

#ifndef _WIN32
// POSIX-specific semantics: exec-failure exit code 127 and /bin/sh below.
// Windows equivalents (CreateProcess failure surfaces as a spawn error)
// belong to the win32 backend's own coverage.
TEST(Integration, FailedSpawnReportsClose) {
    StdioServerParameters parameters;
    parameters.command = "/nonexistent/mcp-server-binary";

    auto transport = std::make_shared<StdioClientTransport>(parameters);

    std::mutex mutex;
    std::condition_variable cv;
    bool closed = false;
    transport->set_close_handler([&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
        }
        cv.notify_all();
    });
    transport->set_message_handler([](Message) {});
    transport->set_error_handler([](Error) {});

    transport->connect();

    // exec fails -> child exits 127 -> stdout EOF -> close event.
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return closed; }));
    lock.unlock();

    transport->disconnect();
    auto status = transport->exit_status();
    ASSERT_TRUE(status.has_value());
    EXPECT_TRUE(pal::exited_normally(*status));
    EXPECT_EQ(pal::exit_code(*status), 127);
}

TEST(Integration, EnvAndArgsArePassedToChild) {
    // Use /bin/sh to verify args/env plumbing without a dedicated helper.
    StdioServerParameters parameters;
    parameters.command = "/bin/sh";
    parameters.args = {"-c", "printf '%s\\n' \"$MCP_TEST_VAR\" >&2; exit 0"};
    parameters.env["MCP_TEST_VAR"] = "hello-from-env";

    auto transport = std::make_shared<StdioClientTransport>(parameters);

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> lines;
    transport->set_stderr_handler([&](std::string line) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            lines.push_back(std::move(line));
        }
        cv.notify_all();
    });
    transport->set_message_handler([](Message) {});
    transport->set_error_handler([](Error) {});
    transport->set_close_handler([] {});

    transport->connect();

    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(
        cv.wait_for(lock, std::chrono::seconds(3), [&] { return !lines.empty(); }));
    EXPECT_EQ(lines[0], "hello-from-env");
    lock.unlock();

    transport->disconnect();
}
#endif  // !_WIN32

}  // namespace
