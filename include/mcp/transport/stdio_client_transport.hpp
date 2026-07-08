#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <mcp/transport/line_transport.hpp>

namespace mcp {

/// Launch configuration for an MCP server subprocess (FR-TRAN-002).
struct StdioServerParameters {
    std::string command;
    std::vector<std::string> args;
    /// Added to (overriding) the inherited environment.
    std::map<std::string, std::string> env;
    std::optional<std::string> cwd;
};

/// Client-side stdio transport: spawns the server as a subprocess and speaks
/// newline-delimited JSON over its stdin/stdout. stderr is captured line by
/// line for logging (FR-TRAN-002). Shutdown escalates per FR-TRAN-004:
/// close stdin → wait → SIGTERM → wait → SIGKILL.
class StdioClientTransport final : public LineTransportBase {
public:
    explicit StdioClientTransport(StdioServerParameters parameters);
    ~StdioClientTransport() override;

    void connect() override;
    void disconnect() override;

    /// Receives each stderr line from the subprocess (informational only).
    void set_stderr_handler(std::function<void(std::string)> handler);

    /// Grace period per escalation step during disconnect.
    void set_shutdown_grace(std::chrono::milliseconds grace) { grace_ = grace; }

    /// Child exit status as reported by waitpid, available after disconnect.
    std::optional<int> exit_status() const;

protected:
    void write_line(const std::string& line) override;

private:
    void stdout_loop();
    void stderr_loop();

    StdioServerParameters parameters_;
    std::chrono::milliseconds grace_{2000};

    std::atomic<bool> running_{false};
    std::atomic<std::int64_t> child_pid_{-1};
    std::atomic<int> exit_status_{-1};
    std::atomic<bool> exited_{false};

    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;

    std::thread stdout_thread_;
    std::thread stderr_thread_;

    std::mutex stderr_handler_mutex_;
    std::function<void(std::string)> stderr_handler_;
};

}  // namespace mcp
