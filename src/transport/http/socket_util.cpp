#include "socket_util.hpp"

#include <cerrno>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mcp::detail::http {

namespace {

struct Addr {
    sockaddr_in sin{};
    socklen_t len = sizeof(sin);
};

sockaddr_in make_addr(const std::string& host, std::uint16_t port) {
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        sin.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        ::inet_pton(AF_INET, host.c_str(), &sin.sin_addr);
    }
    return sin;
}

}  // namespace

int listen_tcp(const std::string& host, std::uint16_t port,
               std::uint16_t& bound_port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    set_cloexec(fd, true);

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    auto sin = make_addr(host, port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 16) != 0) {
        ::close(fd);
        return -1;
    }

    Addr actual{};
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual.sin),
                      &actual.len) == 0) {
        bound_port = ntohs(actual.sin.sin_port);
    } else {
        bound_port = port;
    }
    return fd;
}

int accept_connection(int listen_fd) {
    for (;;) {
        const int fd = ::accept(listen_fd, nullptr, nullptr);
        if (fd >= 0) {
            set_cloexec(fd, true);
            return fd;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
}

int connect_tcp(const std::string& host, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    set_cloexec(fd, true);
    auto sin = make_addr(host, port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

PollResult poll_readable(int fd, int wake_fd, int timeout_ms) {
    pollfd fds[2]{};
    int nfds = 0;
    fds[nfds] = {fd, POLLIN, 0};
    ++nfds;
    if (wake_fd >= 0) {
        fds[nfds] = {wake_fd, POLLIN, 0};
        ++nfds;
    }
    const int rc = ::poll(fds, static_cast<nfds_t>(nfds), timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) {
            // Treat interrupt as a wake check: if the wake fd fired, return Wake.
            return (nfds == 2 && (fds[1].revents & POLLIN)) ? PollResult::Wake
                                                            : PollResult::Timeout;
        }
        return PollResult::Error;
    }
    if (rc == 0) {
        return PollResult::Timeout;
    }
    if (nfds == 2 && (fds[1].revents & POLLIN)) {
        return PollResult::Wake;
    }
    if (fds[0].revents & POLLIN) {
        return PollResult::Ready;
    }
    // Hangup/error without POLLIN: surface as ready so the caller reads and
    // observes EOF/error.
    if (fds[0].revents != 0) {
        return PollResult::Ready;
    }
    return PollResult::Timeout;
}

void set_cloexec(int fd, bool on) {
    if (fd < 0) {
        return;
    }
    int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
        return;
    }
    if (on) {
        flags |= FD_CLOEXEC;
    } else {
        flags &= ~FD_CLOEXEC;
    }
    ::fcntl(fd, F_SETFD, flags);
}

}  // namespace mcp::detail::http