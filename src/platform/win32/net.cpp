// win32 PAL backend: TCP sockets (Winsock 2.2).

#include "win32_common.hpp"

namespace mcp::pal {

void ensure_wsa() {
    static const int rc = [] {
        WSADATA data;
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    (void)rc;
}

namespace {

bool fill_addr(const std::string& host, std::uint16_t port, sockaddr_in& addr,
               std::string& error) {
    addr = sockaddr_in{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    const char* node = host == "localhost" ? "127.0.0.1" : host.c_str();
    if (::inet_pton(AF_INET, node, &addr.sin_addr) != 1) {
        error = "invalid IPv4 address: " + host;
        return false;
    }
    return true;
}

std::string wsa_error() {
    return "winsock error " + std::to_string(::WSAGetLastError());
}

}  // namespace

fd_t tcp_listen(const std::string& host, std::uint16_t port, std::string& error) {
    ensure_wsa();
    sockaddr_in addr;
    if (!fill_addr(host, port, addr, error)) {
        return kInvalidFd;
    }
    const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        error = wsa_error();
        return kInvalidFd;
    }
    // Exclusive bind: SO_REUSEADDR on Windows allows port hijacking.
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 16) != 0) {
        error = wsa_error();
        ::closesocket(fd);
        return kInvalidFd;
    }
    return static_cast<fd_t>(fd);
}

std::uint16_t tcp_local_port(fd_t fd) {
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (::getsockname(static_cast<SOCKET>(fd), reinterpret_cast<sockaddr*>(&addr),
                      &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

fd_t tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms,
                 std::string& error) {
    ensure_wsa();
    sockaddr_in addr;
    if (!fill_addr(host, port, addr, error)) {
        return kInvalidFd;
    }
    const SOCKET fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) {
        error = wsa_error();
        return kInvalidFd;
    }

    u_long nonblocking = 1;
    ::ioctlsocket(fd, FIONBIO, &nonblocking);
    const int rc =
        ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && ::WSAGetLastError() != WSAEWOULDBLOCK) {
        error = wsa_error();
        ::closesocket(fd);
        return kInvalidFd;
    }
    if (rc != 0) {
        WSAPOLLFD pfd{fd, POLLWRNORM, 0};
        const int ready = ::WSAPoll(&pfd, 1, timeout_ms);
        int so_error = 0;
        int len = sizeof(so_error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&so_error), &len);
        if (ready <= 0 || so_error != 0 || (pfd.revents & POLLERR) != 0) {
            error = ready <= 0 ? "connect timed out" : wsa_error();
            ::closesocket(fd);
            return kInvalidFd;
        }
    }
    nonblocking = 0;
    ::ioctlsocket(fd, FIONBIO, &nonblocking);
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&one), sizeof(one));
    return static_cast<fd_t>(fd);
}

fd_t tcp_accept(fd_t listen_fd) {
    const SOCKET fd = ::accept(static_cast<SOCKET>(listen_fd), nullptr, nullptr);
    return fd == INVALID_SOCKET ? kInvalidFd : static_cast<fd_t>(fd);
}

}  // namespace mcp::pal
