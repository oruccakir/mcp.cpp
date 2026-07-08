#pragma once

// Platform-neutral line framing over PAL descriptors, shared by the stdio
// transports.

#include <string>

#include "../platform/pal.hpp"

namespace mcp::detail {

/// Buffered blocking line reader over a PAL descriptor.
class LineReader {
public:
    enum class Status { Line, Eof, IoError };

    explicit LineReader(pal::fd_t fd) : fd_(fd) {}

    /// Blocks until a full line, EOF, or error. `out` receives the line
    /// without its trailing '\n'.
    Status next(std::string& out);

private:
    pal::fd_t fd_;
    std::string buffer_;
    bool eof_ = false;
};

}  // namespace mcp::detail
