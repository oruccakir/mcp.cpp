#pragma once

// Internal HTTP listener shared by HttpServerTransport (sessionless) and
// HttpSessionServer (multi-session): accept loop, per-connection threads,
// request assembly, and the FR-TRAN-008 checks (path/Origin/authorize).

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <mcp/transport/http_server_transport.hpp>  // HttpServerOptions

#include "../../platform/pal.hpp"
#include "http_codec.hpp"

namespace mcp::detail {

class HttpEndpoint {
public:
    /// Invoked on a connection thread once a full request (head + body) has
    /// been read and passed the path/Origin/authorize checks. The handler
    /// must write a complete response to `fd`; the endpoint closes it after.
    using RequestHandler = std::function<void(const HttpHead& head,
                                              const std::string& body, int fd)>;

    HttpEndpoint(HttpServerOptions options, RequestHandler handler);
    ~HttpEndpoint();

    /// Binds and starts the accept loop. Returns false with `error` filled.
    bool start(std::string& error);
    /// Wakes all connections, joins threads, closes sockets. Idempotent.
    void stop();

    std::uint16_t port() const { return bound_port_.load(); }
    bool running() const { return running_.load(); }
    /// Wake event for long polls in handlers (signalled once stop() begins).
    const pal::WakeEvent& wake() const { return wake_; }

    /// Writes a complete plain response (adds MCP-Protocol-Version).
    static void write_simple(int fd, int status, const std::string& reason,
                             const std::string& body = "",
                             const std::string& content_type = "text/plain",
                             const HeaderList& extra_headers = {});

private:
    void accept_loop();
    void handle_connection(int fd);
    bool origin_allowed(const HttpHead& head) const;

    HttpServerOptions options_;
    RequestHandler handler_;
    std::atomic<bool> running_{false};
    pal::fd_t listen_fd_ = pal::kInvalidFd;
    pal::WakeEvent wake_;
    std::atomic<std::uint16_t> bound_port_{0};
    std::thread accept_thread_;

    std::mutex conn_mutex_;
    std::vector<std::thread> conn_threads_;
    std::vector<pal::fd_t> conn_fds_;
};

}  // namespace mcp::detail
