#include "line_io.hpp"

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
        const long n = pal::read_some(fd_, chunk, sizeof(chunk));
        if (n > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(n));
        } else if (n == 0) {
            eof_ = true;
        } else {
            return Status::IoError;
        }
    }
}

}  // namespace mcp::detail
