#include <mcp/transport/http_server_transport.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mcp/error.hpp>
#include <mcp/jsonrpc/message.hpp>
#include <mcp/types.hpp>

#include "http/http_codec.hpp"
#include "http/socket_util.hpp"
#include "line_io.hpp"

namespace mcp {

namespace {

using detail::http::HttpRequest;
using detail::http::HttpRequestParser;
using detail::http::header_get;
using detail::http::lower;
using detail::http::ParseStatus;
using detail::http::PollResult;
using detail::http::poll_readable;
using detail::http::serialize_response;

bool parse_int(std::string_view text, std::size_t& out) {
    if (text.empty()) return false;
    out = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + static_cast<std::size_t>(c - '0');
    }
    return true;
}

/// Per-thread sink capturing JSON-RPC responses emitted synchronously while a
/// POST is being dispatched (FR-TRAN-006). The Session dispatches requests on
/// the connection thread and calls transport->send() for each response from
/// that same thread, so this captures them to answer the POST in-line.
struct PostResponder {
    std::vector<JsonRpcResponse> responses;
};

thread_local PostResponder* tls_post = nullptr;

JsonRpcResponse error_response(std::optional<RequestId> id, const Error& e) {
    JsonRpcResponse r;
    r.id = std::move(id);
    r.error = e;
    return r;
}

bool is_localhost_origin(const std::string& origin) {
    // Origin is "scheme://host[:port]". Accept localhost/loopback hosts.
    const auto scheme = origin.find("://");
    const std::size_t host_start = scheme == std::string::npos ? 0 : scheme + 3;
    std::size_t host_end = origin.find('/', host_start);
    if (host_end == std::string::npos) host_end = origin.size();
    const auto colon = origin.find(':', host_start);
    const std::size_t host_len =
        (colon == std::string::npos || colon > host_end)
            ? (host_end - host_start)
            : (colon - host_start);
    const std::string host = origin.substr(host_start, host_len);
    return host == "localhost" || host == "127.0.0.1" || host == "::1" ||
           host == "0.0.0.0";
}

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

std::string strip_query(const std::string& target) {
    const auto q = target.find('?');
    return q == std::string::npos ? target : target.substr(0, q);
}

/// Reads a complete HTTP request from `fd`, unblockable via `wake_fd`.
bool read_request(int fd, int wake_fd, const std::atomic<bool>& running,
                 HttpRequestParser& parser, HttpRequest& out) {
    for (;;) {
        const PollResult pr = poll_readable(fd, wake_fd, -1);
        if (pr == PollResult::Wake) {
            if (!running.load()) return false;
            continue;
        }
        if (pr == PollResult::Error) {
            return false;
        }
        char chunk[4096];
        const ssize_t n = ::read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) {
            return false;  // peer closed before a complete request
        }
        const auto st = parser.feed(chunk, static_cast<std::size_t>(n));
        if (st == ParseStatus::Done) {
            out = parser.take();
            return true;
        }
        if (st == ParseStatus::Error) {
            return false;
        }
    }
}

void send_simple(int fd, int status, const char* reason) {
    detail::http::HttpResponse res{status, reason, {}, ""};
    res.headers["content-length"] = "0";
    res.headers["connection"] = "close";
    res.headers["mcp-protocol-version"] = kProtocolVersion;
    const std::string wire = serialize_response(res);
    (void)detail::write_all(fd, wire.data(), wire.size());
}

void send_json(int fd, int status, const char* reason,
               const std::string& body) {
    detail::http::HttpResponse res{status, reason, {}, body};
    res.headers["content-type"] = "application/json";
    res.headers["content-length"] = std::to_string(body.size());
    res.headers["connection"] = "close";
    res.headers["mcp-protocol-version"] = kProtocolVersion;
    const std::string wire = serialize_response(res);
    (void)detail::write_all(fd, wire.data(), wire.size());
}

}  // namespace

// A connection thread plus a shared "done" flag so the accept loop can reap
// finished threads without unbounded growth (perf polish is Phase 5+, but a
// leak per request is not acceptable even now).
struct HttpServerTransport::Conn {
    std::thread t;
    std::unique_ptr<std::atomic<bool>> done;
};

HttpServerTransport::HttpServerTransport() = default;

HttpServerTransport::HttpServerTransport(HttpServerOptions options)
    : options_(std::move(options)) {}

HttpServerTransport::~HttpServerTransport() { disconnect(); }

void HttpServerTransport::connect() {
    if (running_.exchange(true)) {
        return;
    }
    if (::pipe(wake_pipe_) != 0) {
        running_.store(false);
        emit_error(Error(ErrorCode::InternalError, "failed to create wake pipe"));
        return;
    }
    detail::http::set_cloexec(wake_pipe_[0], true);
    detail::http::set_cloexec(wake_pipe_[1], true);

    std::uint16_t bound = 0;
    listen_fd_ = detail::http::listen_tcp(options_.host, options_.port, bound);
    if (listen_fd_ < 0) {
        emit_error(Error(ErrorCode::InternalError,
                         "failed to listen on " + options_.host));
        close_fd(wake_pipe_[0]);
        close_fd(wake_pipe_[1]);
        running_.store(false);
        return;
    }
    port_ = bound;
    accept_thread_ = std::thread([this] { accept_loop(); });
}

void HttpServerTransport::disconnect() {
    const bool was = running_.exchange(false);
    if (was) {
        if (wake_pipe_[1] >= 0) {
            const char byte = 0;
            (void)detail::write_all(wake_pipe_[1], &byte, 1);
        }
        {
            std::lock_guard<std::mutex> lock(sse_mutex_);
            ++stream_token_;  // invalidate the active SSE stream
        }
        sse_cv_.notify_all();
    }

    if (accept_thread_.joinable() &&
        accept_thread_.get_id() != std::this_thread::get_id()) {
        accept_thread_.join();
    }

    std::vector<std::unique_ptr<Conn>> conns;
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        conns.swap(conns_);
    }
    for (auto& c : conns) {
        if (c->t.joinable() && c->t.get_id() != std::this_thread::get_id()) {
            c->t.join();
        }
    }

    close_fd(wake_pipe_[0]);
    close_fd(wake_pipe_[1]);
    close_fd(listen_fd_);

    if (was) {
        // Emit close exactly once so a blocked Server::run() (or a session
        // waiting on the close callback) can proceed. Guarded: the Session
        // destructor calls disconnect() again and must not re-fire the close
        // callback (which would double-satisfy a std::promise).
        if (!close_emitted_.exchange(true)) {
            emit_close();
        }
    }
}

void HttpServerTransport::accept_loop() {
    for (;;) {
        const PollResult pr = poll_readable(listen_fd_, wake_pipe_[0], -1);
        if (!running_.load()) {
            return;
        }
        if (pr == PollResult::Wake) {
            return;
        }
        if (pr != PollResult::Ready) {
            continue;
        }
        const int fd = detail::http::accept_connection(listen_fd_);
        if (fd < 0) {
            if (!running_.load()) return;
            continue;
        }
        auto conn = std::make_unique<Conn>();
        conn->done = std::make_unique<std::atomic<bool>>(false);
        auto* done = conn->done.get();
        conn->t = std::thread([this, fd, done] {
            handle_connection(fd);
            done->store(true);
        });
        std::lock_guard<std::mutex> lock(conns_mutex_);
        conns_.push_back(std::move(conn));
        // Opportunistically reap finished connection threads.
        conns_.erase(
            std::remove_if(conns_.begin(), conns_.end(),
                           [](std::unique_ptr<Conn>& c) {
                               if (c->done->load() && c->t.joinable()) {
                                   c->t.join();
                                   return true;
                               }
                               return false;
                           }),
            conns_.end());
    }
}

void HttpServerTransport::handle_connection(int fd) {
    HttpRequestParser parser;
    HttpRequest raw;
    if (!read_request(fd, wake_pipe_[0], running_, parser, raw)) {
        return;
    }
    if (!running_.load()) {
        return;
    }
    HttpRequestView req{raw.method, raw.target, std::move(raw.headers),
                       std::move(raw.body)};
    if (req.method == "POST") {
        handle_post(fd, std::move(req));
    } else if (req.method == "GET") {
        handle_get(fd, std::move(req));
    } else {
        send_simple(fd, 405, "Method Not Allowed");
    }
}

void HttpServerTransport::handle_post(int fd, HttpRequestView req) {
    if (strip_query(req.target) != options_.path) {
        send_simple(fd, 404, "Not Found");
        return;
    }
    // Origin validation (FR-TRAN-008).
    if (const auto* origin = header_get(req.headers, "origin")) {
        bool ok = options_.allowed_origins.empty()
                      ? is_localhost_origin(*origin)
                      : std::find(options_.allowed_origins.begin(),
                                  options_.allowed_origins.end(),
                                  *origin) != options_.allowed_origins.end();
        if (!ok) {
            send_simple(fd, 403, "Forbidden");
            return;
        }
    }
    if (options_.authorize) {
        HttpRequestView view{req.method, strip_query(req.target), req.headers,
                            req.body};
        if (!options_.authorize(view)) {
            send_simple(fd, 401, "Unauthorized");
            return;
        }
    }

    auto frame = parse_frame(req.body);
    if (!frame) {
        auto er = error_response(std::nullopt, frame.error());
        send_json(fd, 400, "Bad Request", serialize_message(Message(er)));
        return;
    }

    const bool was_batch = frame.value().was_batch;
    std::vector<JsonRpcResponse> responses;
    for (const auto& e : frame.value().item_errors) {
        responses.push_back(error_response(std::nullopt, e));
    }

    bool has_request = false;
    for (const auto& m : frame.value().messages) {
        if (std::holds_alternative<JsonRpcRequest>(m)) {
            has_request = true;
            break;
        }
    }

    std::function<void(Message)> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = message_handler_;
    }

    if (has_request) {
        PostResponder resp;
        tls_post = &resp;
        for (auto& m : frame.value().messages) {
            handler(std::move(m));
        }
        tls_post = nullptr;
        for (auto& r : resp.responses) {
            responses.push_back(std::move(r));
        }
    } else {
        for (auto& m : frame.value().messages) {
            handler(std::move(m));
        }
    }

    if (responses.empty()) {
        send_simple(fd, 202, "Accepted");
    } else if (was_batch) {
        json arr = json::array();
        for (const auto& r : responses) {
            arr.push_back(message_to_json(Message(r)));
        }
        send_json(fd, 200, "OK", arr.dump());
    } else {
        send_json(fd, 200, "OK", serialize_message(Message(responses[0])));
    }
}

void HttpServerTransport::handle_get(int fd, HttpRequestView req) {
    if (strip_query(req.target) != options_.path) {
        send_simple(fd, 404, "Not Found");
        return;
    }
    if (const auto* accept = header_get(req.headers, "accept")) {
        if (lower(*accept).find("text/event-stream") == std::string::npos) {
            send_simple(fd, 406, "Not Acceptable");
            return;
        }
    } else {
        send_simple(fd, 406, "Not Acceptable");
        return;
    }
    if (const auto* origin = header_get(req.headers, "origin")) {
        bool ok = options_.allowed_origins.empty()
                      ? is_localhost_origin(*origin)
                      : std::find(options_.allowed_origins.begin(),
                                  options_.allowed_origins.end(),
                                  *origin) != options_.allowed_origins.end();
        if (!ok) {
            send_simple(fd, 403, "Forbidden");
            return;
        }
    }
    if (options_.authorize) {
        HttpRequestView view{req.method, strip_query(req.target), req.headers,
                            req.body};
        if (!options_.authorize(view)) {
            send_simple(fd, 401, "Unauthorized");
            return;
        }
    }

    // Acquire the single active stream, replacing any prior one.
    std::int64_t head = 0;
    std::uint64_t token = acquire_stream(fd, head);

    // Response headers.
    detail::http::HttpResponse res{200, "OK", {}, ""};
    res.headers["content-type"] = "text/event-stream";
    res.headers["cache-control"] = "no-cache";
    res.headers["connection"] = "keep-alive";
    res.headers["mcp-protocol-version"] = kProtocolVersion;
    std::string wire = serialize_response(res);
    if (!detail::write_all(fd, wire.data(), wire.size())) {
        return;
    }
    // Initial event: id + empty data + retry (FR-TRAN-007).
    auto init = detail::http::format_sse_event(
        std::optional<std::string>(std::to_string(head)), "",
        std::optional<std::int64_t>(options_.retry_ms.count()));
    if (!detail::write_all(fd, init.data(), init.size())) {
        return;
    }

    // A fresh stream starts after the current head (no redelivery); a
    // reconnect carrying Last-Event-ID resumes after that id (FR-TRAN-009).
    std::int64_t last_delivered = head;
    if (const auto* leid = header_get(req.headers, "last-event-id")) {
        std::size_t v = 0;
        if (parse_int(*leid, v)) {
            last_delivered = static_cast<std::int64_t>(v);
        }
    }

    stream_loop(fd, token, last_delivered);

    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        if (stream_token_ == token) {
            active_stream_fd_ = -1;
        }
    }
}

std::uint64_t HttpServerTransport::acquire_stream(int fd,
                                                  std::int64_t& head_id) {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    ++stream_token_;
    if (active_stream_fd_ >= 0 && active_stream_fd_ != fd) {
        // Unblocks the prior stream's blocked write; its thread will observe
        // the token change and exit (then close its own fd).
        ::shutdown(active_stream_fd_, SHUT_RDWR);
    }
    active_stream_fd_ = fd;
    head_id = next_event_id_;
    return stream_token_;
}

void HttpServerTransport::enqueue_sse(std::string serialized) {
    if (!running_.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        const std::int64_t id = ++next_event_id_;
        ring_.push_back({id, std::move(serialized)});
        while (ring_.size() > options_.replay_buffer) {
            ring_.pop_front();
        }
    }
    sse_cv_.notify_one();
}

void HttpServerTransport::stream_loop(int fd, std::uint64_t my_token,
                                       std::int64_t last_delivered) {
    for (;;) {
        std::vector<SseEvent> to_send;
        {
            std::unique_lock<std::mutex> lock(sse_mutex_);
            sse_cv_.wait(lock, [&] {
                return !running_.load() || stream_token_ != my_token ||
                       next_event_id_ > last_delivered;
            });
            if (!running_.load() || stream_token_ != my_token) {
                return;
            }
            for (const auto& e : ring_) {
                if (e.id > last_delivered) {
                    to_send.push_back({e.id, e.data});
                }
            }
            last_delivered = next_event_id_;  // caught up (evicted events lost)
        }
        for (const auto& e : to_send) {
            auto frame = detail::http::format_sse_event(
                std::optional<std::string>(std::to_string(e.id)), e.data,
                std::nullopt);
            if (!detail::write_all(fd, frame.data(), frame.size())) {
                return;  // client gone
            }
        }
    }
}

void HttpServerTransport::send(const Message& message) {
    if (std::holds_alternative<JsonRpcResponse>(message)) {
        if (tls_post != nullptr) {
            // Answer the in-flight POST in-line (FR-TRAN-006).
            tls_post->responses.push_back(std::get<JsonRpcResponse>(message));
            return;
        }
        // Late response with no pending POST: deliver via SSE (spec-compliant).
    }
    enqueue_sse(serialize_message(message));
}

void HttpServerTransport::send_batch(const std::vector<Message>& messages) {
    for (const auto& m : messages) {
        send(m);
    }
}

void HttpServerTransport::set_message_handler(
    std::function<void(Message)> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    message_handler_ = std::move(handler);
}

void HttpServerTransport::set_error_handler(
    std::function<void(Error)> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    error_handler_ = std::move(handler);
}

void HttpServerTransport::set_close_handler(std::function<void()> handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    close_handler_ = std::move(handler);
}

void HttpServerTransport::emit_error(const Error& error) {
    std::function<void(Error)> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = error_handler_;
    }
    if (handler) {
        handler(error);
    }
}

void HttpServerTransport::emit_close() {
    std::function<void()> handler;
    {
        std::lock_guard<std::mutex> lock(handler_mutex_);
        handler = close_handler_;
    }
    if (handler) {
        handler();
    }
}

}  // namespace mcp