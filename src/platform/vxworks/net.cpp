// VxWorks 7 RTP backend: TCP sockets (BSD API).
//
// Conservative choices for Wind River userland: FIONBIO via ioctl for
// non-blocking connect (more portable across VxWorks configurations than
// fcntl O_NONBLOCK), plain BSD calls otherwise. See docs/vxworks-port.md.

#include "../pal.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <selectLib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace mcp::pal {

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

bool set_nonblocking(fd_t fd, bool enable) {
    int on = enable ? 1 : 0;
    return ::ioctl(fd, FIONBIO, reinterpret_cast<char*>(&on)) == 0;
}

}  // namespace

fd_t tcp_listen(const std::string& host, std::uint16_t port, std::string& error) {
    sockaddr_in addr;
    if (!fill_addr(host, port, addr, error)) {
        return kInvalidFd;
    }
    const fd_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return kInvalidFd;
    }
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 16) != 0) {
        error = std::strerror(errno);
        ::close(fd);
        return kInvalidFd;
    }
    return fd;
}

std::uint16_t tcp_local_port(fd_t fd) {
    sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

fd_t tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms,
                 std::string& error) {
    sockaddr_in addr;
    if (!fill_addr(host, port, addr, error)) {
        return kInvalidFd;
    }
    const fd_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return kInvalidFd;
    }

    if (!set_nonblocking(fd, true)) {
        error = "FIONBIO failed";
        ::close(fd);
        return kInvalidFd;
    }
    const int rc =
        ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) {
        error = std::strerror(errno);
        ::close(fd);
        return kInvalidFd;
    }
    if (rc != 0) {
        // Wait for writability (connect completion) via select() — poll() is
        // undefined in kernel-module (DKM) builds.
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(fd, &write_set);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        const int ready = ::select(fd + 1, nullptr, &write_set, nullptr, &tv);
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&so_error), &len);
        if (ready <= 0 || so_error != 0) {
            error = ready <= 0 ? "connect timed out" : std::strerror(so_error);
            ::close(fd);
            return kInvalidFd;
        }
    }
    set_nonblocking(fd, false);
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    return fd;
}

fd_t tcp_accept(fd_t listen_fd) {
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    return fd < 0 ? kInvalidFd : fd;
}

}  // namespace mcp::pal
