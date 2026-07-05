#include <mcp/transport/stdio_transport.hpp>

#include <cerrno>

#include <poll.h>
#include <unistd.h>

#include "line_io.hpp"

namespace mcp {

StdioTransport::StdioTransport()
    : in_fd_(STDIN_FILENO), out_fd_(STDOUT_FILENO), owns_fds_(false) {}

StdioTransport::StdioTransport(int in_fd, int out_fd, bool owns_fds)
    : in_fd_(in_fd), out_fd_(out_fd), owns_fds_(owns_fds) {}

StdioTransport::~StdioTransport() { disconnect(); }

void StdioTransport::connect() {
    if (running_.exchange(true)) {
        return;
    }
    if (::pipe(wake_pipe_) != 0) {
        running_.store(false);
        emit_error(Error(ErrorCode::InternalError, "failed to create wake pipe"));
        return;
    }
    read_thread_ = std::thread([this] { read_loop(); });
}

void StdioTransport::disconnect() {
    const bool was_running = running_.exchange(false);
    if (was_running && wake_pipe_[1] >= 0) {
        const char byte = 0;
        (void)detail::write_all(wake_pipe_[1], &byte, 1);
    }
    if (read_thread_.joinable() &&
        read_thread_.get_id() != std::this_thread::get_id()) {
        read_thread_.join();
    }
    for (int* fd : {&wake_pipe_[0], &wake_pipe_[1]}) {
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    }
    if (owns_fds_) {
        if (in_fd_ >= 0) {
            ::close(in_fd_);
            in_fd_ = -1;
        }
        if (out_fd_ >= 0) {
            ::close(out_fd_);
            out_fd_ = -1;
        }
    }
}

void StdioTransport::write_line(const std::string& line) {
    if (out_fd_ < 0) {
        return;
    }
    std::string framed = line;
    framed.push_back('\n');
    if (!detail::write_all(out_fd_, framed.data(), framed.size())) {
        emit_error(Error(ErrorCode::InternalError, "stdio write failed"));
    }
}

void StdioTransport::read_loop() {
    std::string buffer;
    while (running_.load()) {
        struct pollfd fds[2] = {{in_fd_, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
        const int rc = ::poll(fds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            emit_error(Error(ErrorCode::InternalError, "stdio poll failed"));
            break;
        }
        if (fds[1].revents != 0) {
            return;  // disconnect() requested; no close event for local stop
        }
        if (fds[0].revents == 0) {
            continue;
        }

        char chunk[4096];
        const ssize_t n = ::read(in_fd_, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (running_.load()) {
                emit_error(Error(ErrorCode::InternalError, "stdio read failed"));
            }
            break;
        }
        if (n == 0) {
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
