#define MCP_LOG_COMPONENT "stdio"

#include <mcp/transport/stdio_transport.hpp>

#include <mcp/log.hpp>

#include "../platform/pal.hpp"

namespace mcp {

namespace {
// stdin/stdout descriptor values are 0/1 on every supported platform's
// C runtime; the PAL owns anything beyond that assumption.
constexpr int kStdinFd = 0;
constexpr int kStdoutFd = 1;
}  // namespace

StdioTransport::StdioTransport()
    : in_fd_(kStdinFd), out_fd_(kStdoutFd), owns_fds_(false) {}

StdioTransport::StdioTransport(int in_fd, int out_fd, bool owns_fds)
    : in_fd_(in_fd), out_fd_(out_fd), owns_fds_(owns_fds) {}

StdioTransport::~StdioTransport() { disconnect(); }

void StdioTransport::connect() {
    if (running_.exchange(true)) {
        return;
    }
    wake_ = std::make_unique<pal::WakeEvent>();
    if (!wake_->valid()) {
        running_.store(false);
        emit_error(Error(ErrorCode::InternalError, "failed to create wake event"));
        return;
    }
    MCP_LOG(info, "stdio transport connected (in fd " << in_fd_ << ", out fd "
                                                      << out_fd_ << ")");
    read_thread_ = std::thread([this] { read_loop(); });
}

void StdioTransport::disconnect() {
    const bool was_running = running_.exchange(false);
    if (was_running && wake_) {
        wake_->signal();
    }
    if (read_thread_.joinable() &&
        read_thread_.get_id() != std::this_thread::get_id()) {
        read_thread_.join();
    }
    wake_.reset();
    if (owns_fds_) {
        pal::close_fd(in_fd_);
        pal::close_fd(out_fd_);
    }
}

void StdioTransport::write_line(const std::string& line) {
    if (out_fd_ < 0) {
        return;
    }
    std::string framed = line;
    framed.push_back('\n');
    if (!pal::write_all(out_fd_, framed.data(), framed.size())) {
        emit_error(Error(ErrorCode::InternalError, "stdio write failed"));
    }
}

void StdioTransport::read_loop() {
    std::string buffer;
    while (running_.load()) {
        const int ready = pal::poll_readable(in_fd_, wake_.get(), -1);
        if (ready < 0) {
            emit_error(Error(ErrorCode::InternalError, "stdio poll failed"));
            break;
        }
        if (ready == 0) {
            if (!running_.load()) {
                return;  // disconnect() requested; no close event
            }
            continue;
        }

        char chunk[4096];
        const long n = pal::read_some(in_fd_, chunk, sizeof(chunk));
        if (n < 0) {
            if (running_.load()) {
                emit_error(Error(ErrorCode::InternalError, "stdio read failed"));
            }
            break;
        }
        if (n == 0) {
            MCP_LOG(info, "stdin EOF; closing transport");
            break;  // EOF: peer closed
        }

        buffer.append(chunk, static_cast<std::size_t>(n));
        std::size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            process_line(line);
        }
    }
    running_.store(false);
    emit_close();
}

}  // namespace mcp
