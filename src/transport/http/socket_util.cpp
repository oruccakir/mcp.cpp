#include "socket_util.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mcp::detail {

namespace {

bool fill_addr(const std::string& host, std::uint16_t port, sockaddr_in& addr,
               std::string& error) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const char* node = host == "localhost" ? "127.0.0.1" : host.c_str();
    if (::inet_pton(AF_INET, node, &addr.sin_addr) != 1) {
        error = "invalid IPv4 address: " + host;
        return false;
    }
    return true;
}

}  // namespace

int listen_tcp(const std::string& host, std::uint16_t port, std::string& error) {
    sockaddr_in addr;
    if (!fill_addr(host, port, addr, error)) {
        return -1;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 16) != 0) {
        error = std::strerror(errno);
        ::close(fd);
        return -1;
    }
    return fd;
}

std::uint16_t local_port(int fd) {
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

int connect_tcp(const std::string& host, std::uint16_t port, int timeout_ms,
                std::string& error) {
    sockaddr_in addr;
    if (!fill_addr(host, port, addr, error)) {
        return -1;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return -1;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int rc =
        ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        error = std::strerror(errno);
        ::close(fd);
        return -1;
    }
    if (rc != 0) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        const int ready = ::poll(&pfd, 1, timeout_ms);
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
        if (ready <= 0 || so_error != 0) {
            error = ready <= 0 ? "connect timed out" : std::strerror(so_error);
            ::close(fd);
            return -1;
        }
    }
    ::fcntl(fd, F_SETFL, flags);
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

int poll_readable(int fd, int wake_fd, int timeout_ms) {
    struct pollfd fds[2] = {{fd, POLLIN, 0}, {wake_fd, POLLIN, 0}};
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

long read_some(int fd, char* buffer, std::size_t size) {
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

}  // namespace mcp::detail
