#include "line_io.hpp"

#include <cerrno>

#include <unistd.h>

namespace mcp::detail {

LineReader::Status LineReader::next(std::string& out) {
    for (;;) {
        if (auto pos = buffer_.find('\n'); pos != std::string::npos) {
            out = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);
            if (!out.empty() && out.back() == '\r') {
                out.pop_back();
            }
            return Status::Line;
        }
        if (eof_) {
            // Deliver a final unterminated line if one is buffered.
            if (!buffer_.empty()) {
                out = std::move(buffer_);
                buffer_.clear();
                return Status::Line;
            }
            return Status::Eof;
        }

        char chunk[4096];
        const ssize_t n = ::read(fd_, chunk, sizeof(chunk));
        if (n > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(n));
        } else if (n == 0) {
            eof_ = true;
        } else if (errno == EINTR) {
            continue;
        } else {
            return Status::IoError;
        }
    }
}

bool write_all(int fd, const char* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace mcp::detail
