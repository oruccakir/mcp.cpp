#pragma once

#include <atomic>
#include <memory>
#include <mcp/sys/threading.hpp>

#include <mcp/transport/line_transport.hpp>

namespace mcp::pal {
class WakeEvent;
}

namespace mcp {

/// Server-side stdio transport (FR-TRAN-002): newline-delimited JSON read
/// from stdin on a dedicated thread (FR-CONC-004), written to stdout.
/// Custom descriptors are supported for testing and embedding. disconnect()
/// wakes the reader via the PAL wake event, so it never blocks on a quiet
/// peer.
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
    std::unique_ptr<pal::WakeEvent> wake_;
    std::atomic<bool> running_{false};
    mcp::sys::thread read_thread_;
};

}  // namespace mcp
