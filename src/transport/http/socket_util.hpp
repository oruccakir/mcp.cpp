#pragma once

// Internal POSIX TCP helpers for the Streamable HTTP transport.

#include <cstdint>
#include <string>

namespace mcp::detail {

/// Binds and listens on host:port (IPv4, SO_REUSEADDR). port 0 picks an
/// ephemeral port. Returns the listening fd, or -1 with `error` filled.
int listen_tcp(const std::string& host, std::uint16_t port, std::string& error);

/// The locally bound port of a socket (0 on failure).
std::uint16_t local_port(int fd);

/// Blocking connect with timeout. Returns fd or -1 with `error` filled.
int connect_tcp(const std::string& host, std::uint16_t port, int timeout_ms,
                std::string& error);

/// Polls `fd` for readability alongside a wake fd (pass -1 for none).
/// Returns 1 = fd readable, 0 = woken or timed out, -1 = error.
int poll_readable(int fd, int wake_fd, int timeout_ms);

/// read() with EINTR retry. Returns bytes read, 0 on EOF, -1 on error.
long read_some(int fd, char* buffer, std::size_t size);

}  // namespace mcp::detail
