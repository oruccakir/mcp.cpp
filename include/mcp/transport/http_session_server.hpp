#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <mcp/core/session.hpp>
#include <mcp/transport/http_server_transport.hpp>  // HttpServerOptions

namespace mcp::detail {
class HttpEndpoint;
struct HttpHead;
class SessionRegistry;
}  // namespace mcp::detail

namespace mcp {

/// Options for the multi-session Streamable HTTP server.
struct HttpSessionServerOptions {
    HttpServerOptions http;
    /// Sessions idle longer than this are evicted (subsequent requests get
    /// 404 and the client re-initializes). Zero = never expire.
    std::chrono::milliseconds session_idle_timeout{30 * 60 * 1000};
    /// Honor client DELETE to terminate a session; when false answer 405.
    bool allow_client_termination = true;
};

/// Production Streamable HTTP server with Mcp-Session-Id management: each
/// initialize creates a new MCP session (id returned in the Mcp-Session-Id
/// response header); subsequent POST/GET/DELETE must echo the id. Missing
/// id -> 400, unknown/expired -> 404 (client re-initializes), DELETE
/// terminates. Each session has its own ServerSession, SSE stream, and
/// event-id/replay space.
///
/// The factory creates the per-session ServerSession bound to the provided
/// transport — with the Server facade:
///
///   mcp::HttpSessionServer http(options,
///       [&server](std::shared_ptr<mcp::Transport> t) {
///           auto s = std::make_unique<mcp::ServerSession>(
///               std::move(t), server.server_options());
///           server.attach(*s);
///           return s;
///       },
///       [&server](mcp::ServerSession& s) { server.detach(s); });
class HttpSessionServer {
public:
    using SessionFactory = std::function<std::unique_ptr<ServerSession>(
        std::shared_ptr<Transport> transport)>;
    using SessionClosed = std::function<void(ServerSession& session)>;

    HttpSessionServer(HttpSessionServerOptions options, SessionFactory factory,
                      SessionClosed on_session_closed = nullptr);
    ~HttpSessionServer();

    HttpSessionServer(const HttpSessionServer&) = delete;
    HttpSessionServer& operator=(const HttpSessionServer&) = delete;

    /// Binds and serves. Returns false with `error` filled on failure.
    bool start(std::string& error);
    /// Terminates all sessions and stops the listener. Idempotent.
    void stop();

    std::uint16_t port() const;
    std::size_t session_count() const;

private:
    void handle_request(const detail::HttpHead& head, const std::string& body,
                        int fd);

    HttpSessionServerOptions options_;
    SessionFactory factory_;
    SessionClosed on_session_closed_;
    std::unique_ptr<detail::SessionRegistry> registry_;
    std::unique_ptr<detail::HttpEndpoint> endpoint_;
};

}  // namespace mcp
