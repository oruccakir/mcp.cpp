// VxWorks 7 RTP backend: descriptor I/O, WakeEvent (loopback socket pair),
// poll.
//
// Written for user-mode RTPs. WakeEvent uses a connected loopback TCP pair
// rather than pipe(): VxWorks pipe() needs the pipe driver component in the
// kernel image (often absent in RTP images — it fails at runtime with
// "failed to create wake event"), whereas the socket stack is already a
// hard requirement of the HTTP transport. Same approach as the win32
// backend. See docs/vxworks-port.md.

#include "../pal.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
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

namespace {

// Connected loopback TCP pair (VxWorks lacks a reliable socketpair() for
// AF_INET). Returns false on any failure.
bool make_loopback_pair(fd_t& read_side, fd_t& write_side) {
    read_side = write_side = kInvalidFd;
    const fd_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return false;
    }
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t len = sizeof(addr);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(listener);
        return false;
    }
    const fd_t connector = ::socket(AF_INET, SOCK_STREAM, 0);
    if (connector < 0 ||
        ::connect(connector, reinterpret_cast<sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
        if (connector >= 0) {
            ::close(connector);
        }
        ::close(listener);
        return false;
    }
    const fd_t accepted = ::accept(listener, nullptr, nullptr);
    ::close(listener);
    if (accepted < 0) {
        ::close(connector);
        return false;
    }
    read_side = accepted;    // polled side
    write_side = connector;  // signalled side
    return true;
}

}  // namespace

WakeEvent::WakeEvent() {
    if (!make_loopback_pair(fds_[0], fds_[1])) {
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
