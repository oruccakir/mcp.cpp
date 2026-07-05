#pragma once

// Internal POSIX fd line I/O helpers shared by the stdio transports.

#include <string>

namespace mcp::detail {

/// Buffered blocking line reader over a POSIX file descriptor.
class LineReader {
public:
    enum class Status { Line, Eof, IoError };

    explicit LineReader(int fd) : fd_(fd) {}

    /// Blocks until a full line, EOF, or error. `out` receives the line
    /// without its trailing '\n'.
    Status next(std::string& out);

private:
    int fd_;
    std::string buffer_;
    bool eof_ = false;
};

/// Writes the full buffer, retrying on partial writes/EINTR.
bool write_all(int fd, const char* data, std::size_t size);

}  // namespace mcp::detail
