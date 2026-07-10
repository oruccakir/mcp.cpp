// VxWorks 7 RTP backend: descriptor I/O, WakeEvent (self-pipe), poll.
//
// Written for user-mode RTPs, where VxWorks provides the POSIX surface used
// below (pipe/read/write/close/poll). Deliberately conservative: mirrors the
// posix backend with VxWorks-safe choices only. First compiled on an
// air-gapped Wind River toolchain — if something here fails to build, see
// docs/vxworks-port.md for the report-back checklist.

#include "../pal.hpp"

#include <cerrno>
#include <csignal>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mcp::pal {

long read_some(fd_t fd, char* buffer, std::size_t size) {
    for (;;) {
        const ssize_t n = ::read(fd, buffer, size);
        if (n >= 0) {
            return static_cast<long>(n);
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

bool write_all(fd_t fd, const char* data, std::size_t size) {
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

void close_fd(fd_t& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = kInvalidFd;
    }
}

void shutdown_fd(fd_t fd) {
    if (fd >= 0) {
        // shutdown() is declared in sockLib for sockets; harmless failure on
        // non-socket descriptors.
        ::shutdown(fd, SHUT_RDWR);
    }
}

WakeEvent::WakeEvent() {
    if (::pipe(fds_) != 0) {
        fds_[0] = fds_[1] = kInvalidFd;
    }
}

WakeEvent::~WakeEvent() {
    close_fd(fds_[0]);
    close_fd(fds_[1]);
}

bool WakeEvent::valid() const { return fds_[0] >= 0; }

void WakeEvent::signal() {
    if (fds_[1] >= 0) {
        const char byte = 0;
        (void)write_all(fds_[1], &byte, 1);
    }
}

fd_t WakeEvent::poll_handle() const { return fds_[0]; }

int poll_readable(fd_t fd, const WakeEvent* wake, int timeout_ms) {
    const fd_t wake_fd = wake ? wake->poll_handle() : kInvalidFd;
    struct pollfd fds[2];
    fds[0].fd = fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = wake_fd;
    fds[1].events = POLLIN;
    fds[1].revents = 0;
    const nfds_t count = wake_fd >= 0 ? 2 : 1;
    for (;;) {
        const int rc = ::poll(fds, count, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (rc == 0) {
            return 0;  // timeout
        }
        if (count == 2 && fds[1].revents != 0) {
            return 0;  // woken
        }
        if (fds[0].revents != 0) {
            return 1;
        }
    }
}

void ignore_broken_pipe_signals() {
#if defined(SIGPIPE)
    ::signal(SIGPIPE, SIG_IGN);
#endif
}

}  // namespace mcp::pal
