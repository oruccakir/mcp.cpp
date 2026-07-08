// win32 PAL backend: descriptor I/O, WakeEvent (loopback socket pair), poll.
//
// An fd_t carries either a SOCKET (from pal::tcp_*) or a CRT file
// descriptor (pipes/stdio); is_socket() disambiguates.

#include "win32_common.hpp"

#include <chrono>
#include <thread>

#include <io.h>

namespace mcp::pal {

bool is_socket(fd_t fd) {
    if (fd < 0) {
        return false;
    }
    ensure_wsa();
    int type = 0;
    int length = sizeof(type);
    return ::getsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_TYPE,
                        reinterpret_cast<char*>(&type), &length) == 0;
}

long read_some(fd_t fd, char* buffer, std::size_t size) {
    if (is_socket(fd)) {
        const int n = ::recv(static_cast<SOCKET>(fd), buffer,
                             static_cast<int>(size), 0);
        if (n == SOCKET_ERROR) {
            // A locally shut-down/closed socket reads as EOF for our
            // transports' purposes.
            const int err = ::WSAGetLastError();
            return err == WSAECONNRESET || err == WSAESHUTDOWN ? 0 : -1;
        }
        return n;
    }
    const int n = ::_read(static_cast<int>(fd), buffer,
                          static_cast<unsigned>(size));
    return n < 0 ? -1 : n;
}

bool write_all(fd_t fd, const char* data, std::size_t size) {
    std::size_t written = 0;
    if (is_socket(fd)) {
        while (written < size) {
            const int n = ::send(static_cast<SOCKET>(fd), data + written,
                                 static_cast<int>(size - written), 0);
            if (n == SOCKET_ERROR) {
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        return true;
    }
    while (written < size) {
        const int n = ::_write(static_cast<int>(fd), data + written,
                               static_cast<unsigned>(size - written));
        if (n < 0) {
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    return true;
}

void close_fd(fd_t& fd) {
    if (fd < 0) {
        return;
    }
    if (is_socket(fd)) {
        ::closesocket(static_cast<SOCKET>(fd));
    } else {
        ::_close(static_cast<int>(fd));
    }
    fd = kInvalidFd;
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::_close(fd);  // int descriptors are CRT fds by construction
        fd = -1;
    }
}

void shutdown_fd(fd_t fd) {
    if (fd >= 0 && is_socket(fd)) {
        ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
    }
}

// ---------------------------------------------------------------- WakeEvent

namespace {

/// Connected loopback socket pair so WSAPoll can wait on wake signals.
bool make_socket_pair(SOCKET& a, SOCKET& b) {
    a = b = INVALID_SOCKET;
    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int len = sizeof(addr);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::closesocket(listener);
        return false;
    }
    a = ::socket(AF_INET, SOCK_STREAM, 0);
    if (a == INVALID_SOCKET ||
        ::connect(a, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(listener);
        if (a != INVALID_SOCKET) {
            ::closesocket(a);
            a = INVALID_SOCKET;
        }
        return false;
    }
    b = ::accept(listener, nullptr, nullptr);
    ::closesocket(listener);
    if (b == INVALID_SOCKET) {
        ::closesocket(a);
        a = INVALID_SOCKET;
        return false;
    }
    return true;
}

bool socket_readable_now(SOCKET s) {
    WSAPOLLFD p{s, POLLRDNORM, 0};
    return ::WSAPoll(&p, 1, 0) == 1;
}

}  // namespace

WakeEvent::WakeEvent() {
    ensure_wsa();
    SOCKET read_side = INVALID_SOCKET;
    SOCKET write_side = INVALID_SOCKET;
    if (make_socket_pair(write_side, read_side)) {
        fds_[0] = static_cast<fd_t>(read_side);   // polled side
        fds_[1] = static_cast<fd_t>(write_side);  // signal side
    }
}

WakeEvent::~WakeEvent() {
    for (fd_t& fd : fds_) {
        if (fd >= 0) {
            ::closesocket(static_cast<SOCKET>(fd));
            fd = kInvalidFd;
        }
    }
}

bool WakeEvent::valid() const { return fds_[0] >= 0; }

void WakeEvent::signal() {
    if (fds_[1] >= 0) {
        const char byte = 0;
        (void)::send(static_cast<SOCKET>(fds_[1]), &byte, 1, 0);
    }
}

fd_t WakeEvent::poll_handle() const { return fds_[0]; }

// ------------------------------------------------------------------- poll

int poll_readable(fd_t fd, const WakeEvent* wake, int timeout_ms) {
    ensure_wsa();
    const SOCKET wake_socket =
        wake && wake->poll_handle() >= 0
            ? static_cast<SOCKET>(wake->poll_handle())
            : INVALID_SOCKET;

    if (is_socket(fd)) {
        WSAPOLLFD fds[2] = {{static_cast<SOCKET>(fd), POLLRDNORM, 0},
                            {wake_socket, POLLRDNORM, 0}};
        const ULONG count = wake_socket != INVALID_SOCKET ? 2 : 1;
        const int rc = ::WSAPoll(fds, count, timeout_ms);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            return 0;  // timeout
        }
        if (count == 2 && fds[1].revents != 0) {
            return 0;  // woken
        }
        return fds[0].revents != 0 ? 1 : 0;
    }

    // CRT descriptor (pipe or console): no WSAPoll — check readiness in
    // slices, watching the wake socket between checks.
    const HANDLE handle =
        reinterpret_cast<HANDLE>(::_get_osfhandle(static_cast<int>(fd)));
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    const DWORD type = ::GetFileType(handle);
    const auto deadline =
        timeout_ms < 0
            ? (std::chrono::steady_clock::time_point::max)()
            : std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_ms);
    constexpr int kSliceMs = 20;

    for (;;) {
        if (wake_socket != INVALID_SOCKET && socket_readable_now(wake_socket)) {
            return 0;  // woken
        }
        if (type == FILE_TYPE_PIPE) {
            DWORD available = 0;
            if (!::PeekNamedPipe(handle, nullptr, 0, nullptr, &available,
                                 nullptr)) {
                return 1;  // broken pipe: let the read observe EOF
            }
            if (available > 0) {
                return 1;
            }
        } else {
            // Console/character device: readable when the handle signals.
            const DWORD waited = ::WaitForSingleObject(handle, kSliceMs);
            if (waited == WAIT_OBJECT_0) {
                return 1;
            }
            if (waited == WAIT_FAILED) {
                return -1;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return 0;
            }
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs));
    }
}

void ignore_broken_pipe_signals() {
    // No SIGPIPE on Windows; failed writes already surface as errors.
}

}  // namespace mcp::pal
