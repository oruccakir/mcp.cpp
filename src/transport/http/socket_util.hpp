#pragma once

// Internal POSIX TCP socket helpers for the HTTP transports (FR-TRAN-005..009).
// Hand-rolled and dependency-free, mirroring the stdio transport's POSIX-first
// approach (Linux/macOS now; Windows sockets are a later phase).
//
// Not installed: consumed only by http_server_transport.cpp and
// http_client_transport.cpp.

#include <cstdint>
#include <string>

namespace mcp::detail::http {

/// Result of a poll on one readable fd plus an optional wake fd.
enum class PollResult {
    Ready,     // the primary fd is readable
    Wake,      // the wake fd fired (disconnect requested)
    Timeout,   // neither fd became readable in time
    Error       // poll() failed
};

/// Creates a listening IPv4 TCP socket bound to `host:port`. With port == 0 the
/// kernel picks an ephemeral port, returned in `bound_port`. SO_REUSEADDR is set
/// so test servers can rebind immediately. Returns the fd or -1 on failure.
int listen_tcp(const std::string& host, std::uint16_t port,
               std::uint16_t& bound_port);

/// Blocks until a connection arrives on `listen_fd`. Returns the client fd or
/// -1 on error (including EINTR).
int accept_connection(int listen_fd);

/// Connects a TCP socket to `host:port`. Returns the fd or -1 on failure.
int connect_tcp(const std::string& host, std::uint16_t port);

/// Polls `fd` for readability, unblockable by `wake_fd` (a self-pipe read end).
/// `wake_fd` < 0 disables the wake channel. `timeout_ms` < 0 means wait forever.
PollResult poll_readable(int fd, int wake_fd, int timeout_ms);

/// Sets or clears close-on-exec on `fd`.
void set_cloexec(int fd, bool on);

}  // namespace mcp::detail::http