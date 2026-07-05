#pragma once

#include <atomic>
#include <thread>

#include <mcp/transport/line_transport.hpp>

namespace mcp {

/// Server-side stdio transport (FR-TRAN-002): newline-delimited JSON read
/// from stdin on a dedicated thread (FR-CONC-004), written to stdout.
/// Custom descriptors are supported for testing and embedding. disconnect()
/// wakes the reader via a self-pipe, so it never blocks on a quiet peer.
class StdioTransport final : public LineTransportBase {
public:
    /// Uses stdin/stdout; the descriptors are not closed on disconnect.
    StdioTransport();
    /// Uses the given descriptors; when `owns_fds` is true they are closed
    /// on disconnect/destruction.
    StdioTransport(int in_fd, int out_fd, bool owns_fds = true);
    ~StdioTransport() override;

    void connect() override;
    void disconnect() override;

protected:
    void write_line(const std::string& line) override;

private:
    void read_loop();

    int in_fd_;
    int out_fd_;
    bool owns_fds_;
    int wake_pipe_[2] = {-1, -1};
    std::atomic<bool> running_{false};
    std::thread read_thread_;
};

}  // namespace mcp
