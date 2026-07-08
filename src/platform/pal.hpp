#pragma once

// Platform Abstraction Layer (PAL).
//
// The single API through which transports touch the operating system
// (SRS §3.1 "no platform-only APIs", FR-EMB-002). One backend is selected at
// build time in src/CMakeLists.txt — link-time dispatch, no vtables, no
// scattered #ifdefs. Backends: posix/ (Linux, macOS); win32/, vxworks/,
// freertos/ are future additions with the same signatures.
//
// Keep this surface *narrow*: a function belongs here only when a transport
// actually needs it and it genuinely differs between platforms.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mcp::pal {

/// Descriptor type for sockets and pipes. POSIX: plain fd. Windows: holds
/// either a SOCKET or a CRT file descriptor (pipes/stdio); the backend
/// disambiguates. This alias is the only platform-conditional type in the
/// SDK.
#if defined(_WIN32)
using fd_t = std::intptr_t;
#else
using fd_t = int;
#endif
inline constexpr fd_t kInvalidFd = -1;

// --- Generic descriptor I/O -----------------------------------------------

/// read() with retry on interruption. >0 bytes, 0 EOF, -1 error.
long read_some(fd_t fd, char* buffer, std::size_t size);

/// Writes the whole buffer, retrying partial writes/interruptions.
bool write_all(fd_t fd, const char* data, std::size_t size);

/// Closes and invalidates. No-op on kInvalidFd.
void close_fd(fd_t& fd);
#if defined(_WIN32)
/// Overload for int-typed CRT descriptors (pipes/stdio held by transports);
/// on POSIX fd_t is already int so the primary overload covers this.
void close_fd(int& fd);
#endif

/// Shuts down both directions (sockets); safe on non-sockets.
void shutdown_fd(fd_t fd);

// --- Cross-thread wakeup ----------------------------------------------------

/// Unblocks poll_readable() waits from another thread. POSIX: self-pipe.
/// signal() is sticky: once signalled, every waiter wakes until destruction
/// (used exactly once, at shutdown).
class WakeEvent {
public:
    WakeEvent();
    ~WakeEvent();
    WakeEvent(const WakeEvent&) = delete;
    WakeEvent& operator=(const WakeEvent&) = delete;

    bool valid() const;
    void signal();
    /// Handle a caller may pass as the *main* fd to poll_readable to wait on
    /// the wake signal itself (interruptible sleep).
    fd_t poll_handle() const;

private:
    fd_t fds_[2] = {kInvalidFd, kInvalidFd};
};

/// Waits for readability. Returns 1 = fd readable, 0 = wake signalled or
/// timed out, -1 = error. `wake` may be null; timeout_ms < 0 = infinite.
int poll_readable(fd_t fd, const WakeEvent* wake, int timeout_ms);

// --- TCP (Streamable HTTP transport) ---------------------------------------

/// Binds and listens (IPv4, address reuse). port 0 = ephemeral.
/// Returns kInvalidFd with `error` filled on failure.
fd_t tcp_listen(const std::string& host, std::uint16_t port, std::string& error);

/// Locally bound port of a socket (0 on failure).
std::uint16_t tcp_local_port(fd_t fd);

/// Blocking connect with timeout; kInvalidFd + `error` on failure.
fd_t tcp_connect(const std::string& host, std::uint16_t port, int timeout_ms,
                 std::string& error);

/// Accepts one connection; kInvalidFd on failure/shutdown.
fd_t tcp_accept(fd_t listen_fd);

/// Writing to a dead peer must fail the write, not kill the process
/// (POSIX: ignore SIGPIPE; no-op on platforms without it).
void ignore_broken_pipe_signals();

// --- Child processes (host platforms only; stdio client transport) ----------
// Not available on VxWorks/FreeRTOS backends — the stdio *client* transport
// is host-only by nature (FR-TRAN-002).

struct ProcessSpec {
    std::string command;
    std::vector<std::string> args;
    /// Added to (overriding) the inherited environment.
    std::map<std::string, std::string> env;
    std::optional<std::string> cwd;
};

struct Process {
    std::int64_t pid = -1;
    /// Backend-owned handle (win32: process HANDLE; unused on POSIX).
    std::intptr_t native_handle = 0;
    fd_t stdin_fd = kInvalidFd;
    fd_t stdout_fd = kInvalidFd;
    fd_t stderr_fd = kInvalidFd;
};

/// Spawns with piped stdio. False + `error` on failure.
bool spawn(const ProcessSpec& spec, Process& out, std::string& error);

/// Waits up to timeout_ms for exit; true when exited (`status` = raw
/// platform status: waitpid semantics on POSIX, exit code on win32 — use
/// the helpers below to interpret it portably). False on timeout.
bool wait_exit(Process& process, int timeout_ms, int& status);

/// Asks the child to stop, or forces it. POSIX: SIGTERM / SIGKILL. Windows
/// has no graceful-termination signal: both map to TerminateProcess.
void terminate(Process& process, bool force);

/// Portable interpretation of a `wait_exit` status.
bool exited_normally(int status);
int exit_code(int status);

}  // namespace mcp::pal
