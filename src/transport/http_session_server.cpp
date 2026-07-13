#define MCP_LOG_COMPONENT "http"

#include <mcp/transport/http_session_server.hpp>

#include <atomic>
#include <cstdio>
#include <map>
#include <mcp/sys/threading.hpp>
#include <random>
#include <vector>

#include <mcp/log.hpp>
#include <mcp/methods.hpp>
#include <mcp/types.hpp>

#include "http/http_endpoint.hpp"
#include "http/sse_channel.hpp"

namespace mcp {

namespace detail {

namespace {

std::string generate_session_id() {
    // 128 bits of randomness, hex-encoded (visible-ASCII per spec).
    std::random_device device;
    char out[33];
    for (int i = 0; i < 4; ++i) {
        std::snprintf(out + i * 8, 9, "%08x", device());
    }
    return std::string(out, 32);
}

}  // namespace

/// Transport endpoint for one HTTP session: responses to in-flight POSTs are
/// captured; everything else flows to the session's SSE channel.
class SessionTransport final : public Transport {
public:
    SessionTransport(std::size_t replay_buffer_size, int retry_ms)
        : channel_(replay_buffer_size, retry_ms) {}

    SseChannel& channel() { return channel_; }

    void connect() override {}
    void disconnect() override {
        if (closed_.exchange(true)) {
            return;
        }
        channel_.shutdown();
        std::function<void()> handler;
        {
            std::lock_guard<mcp::sys::mutex> lock(mutex_);
            handler = close_handler_;
        }
        if (handler) {
            handler();
        }
    }

    void send(const Message& message) override {
        if (PostCapture::try_capture(message)) {
            return;
        }
        channel_.enqueue(serialize_message(message));
    }
    void send_batch(const std::vector<Message>& messages) override {
        for (const auto& message : messages) {
            send(message);
        }
    }

    void set_message_handler(std::function<void(Message)> handler) override {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        message_handler_ = std::move(handler);
    }
    void set_error_handler(std::function<void(Error)> handler) override {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        error_handler_ = std::move(handler);
    }
    void set_close_handler(std::function<void()> handler) override {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        close_handler_ = std::move(handler);
    }

    void deliver(Message message) {
        std::function<void(Message)> handler;
        {
            std::lock_guard<mcp::sys::mutex> lock(mutex_);
            handler = message_handler_;
        }
        if (handler) {
            handler(std::move(message));
        }
    }

private:
    SseChannel channel_;
    std::atomic<bool> closed_{false};
    mcp::sys::mutex mutex_;
    std::function<void(Message)> message_handler_;
    std::function<void(Error)> error_handler_;
    std::function<void()> close_handler_;
};

/// Owns the Mcp-Session-Id -> session table.
class SessionRegistry {
public:
    struct Entry {
        std::shared_ptr<SessionTransport> transport;
        std::unique_ptr<ServerSession> session;
        std::chrono::steady_clock::time_point last_activity;
    };
    using EntryPtr = std::shared_ptr<Entry>;

    EntryPtr create(const std::string& id, EntryPtr entry) {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        entries_[id] = entry;
        return entry;
    }

    /// Looks up and touches; nullptr when unknown.
    EntryPtr find(const std::string& id) {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        const auto it = entries_.find(id);
        if (it == entries_.end()) {
            return nullptr;
        }
        it->second->last_activity = std::chrono::steady_clock::now();
        return it->second;
    }

    EntryPtr remove(const std::string& id) {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        const auto it = entries_.find(id);
        if (it == entries_.end()) {
            return nullptr;
        }
        auto entry = std::move(it->second);
        entries_.erase(it);
        return entry;
    }

    std::vector<EntryPtr> remove_idle(std::chrono::milliseconds timeout) {
        std::vector<EntryPtr> evicted;
        if (timeout.count() <= 0) {
            return evicted;
        }
        const auto cutoff = std::chrono::steady_clock::now() - timeout;
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second->last_activity < cutoff) {
                evicted.push_back(std::move(it->second));
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
        return evicted;
    }

    std::vector<EntryPtr> remove_all() {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        std::vector<EntryPtr> all;
        all.reserve(entries_.size());
        for (auto& [id, entry] : entries_) {
            all.push_back(std::move(entry));
        }
        entries_.clear();
        return all;
    }

    std::size_t size() const {
        std::lock_guard<mcp::sys::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    mutable mcp::sys::mutex mutex_;
    std::map<std::string, EntryPtr> entries_;
};

}  // namespace detail

namespace {

constexpr const char* kSessionHeader = "mcp-session-id";

/// Session ids are secrets: log only a prefix.
[[maybe_unused]] std::string id_prefix(const std::string& id) {
    return id.size() > 8 ? id.substr(0, 8) + "..." : id;
}

bool frame_contains_initialize(const ParsedFrame& frame) {
    for (const auto& message : frame.messages) {
        if (const auto* request = std::get_if<JsonRpcRequest>(&message)) {
            if (request->method == methods::kInitialize) {
                return true;
            }
        }
    }
    return false;
}

void write_jsonrpc_error(std::intptr_t fd, int status, const std::string& reason,
                         const Error& error) {
    JsonRpcResponse response;
    response.error = error;
    detail::HttpEndpoint::write_simple(fd, status, reason,
                                       serialize_message(Message(response)),
                                       "application/json");
}

}  // namespace

HttpSessionServer::HttpSessionServer(HttpSessionServerOptions options,
                                     SessionFactory factory,
                                     SessionClosed on_session_closed)
    : options_(std::move(options)),
      factory_(std::move(factory)),
      on_session_closed_(std::move(on_session_closed)),
      registry_(std::make_unique<detail::SessionRegistry>()) {}

HttpSessionServer::~HttpSessionServer() { stop(); }

std::uint16_t HttpSessionServer::port() const {
    return endpoint_ ? endpoint_->port() : 0;
}

std::size_t HttpSessionServer::session_count() const {
    return registry_->size();
}

bool HttpSessionServer::start(std::string& error) {
    if (endpoint_) {
        return true;
    }
    if (!factory_) {
        error = "HttpSessionServer requires a session factory";
        return false;
    }
    endpoint_ = std::make_unique<detail::HttpEndpoint>(
        options_.http,
        [this](const detail::HttpHead& head, const std::string& body, std::intptr_t fd) {
            handle_request(head, body, fd);
        });
    if (!endpoint_->start(error)) {
        endpoint_.reset();
        return false;
    }
    return true;
}

void HttpSessionServer::stop() {
    if (!endpoint_) {
        return;
    }
    // Wake session streams first, then stop the listener (joins connection
    // threads), then notify about destroyed sessions.
    auto all = registry_->remove_all();
    for (auto& entry : all) {
        entry->transport->disconnect();
    }
    endpoint_->stop();
    for (auto& entry : all) {
        if (on_session_closed_ && entry->session) {
            on_session_closed_(*entry->session);
        }
    }
    all.clear();
    endpoint_.reset();
}

void HttpSessionServer::handle_request(const detail::HttpHead& head,
                                       const std::string& body, std::intptr_t fd) {
    // Lazy idle sweep on request traffic.
    for (auto& evicted : registry_->remove_idle(options_.session_idle_timeout)) {
        MCP_LOG(info, "session expired (idle)");
        evicted->transport->disconnect();
        if (on_session_closed_ && evicted->session) {
            on_session_closed_(*evicted->session);
        }
    }

    const std::string session_id = head.header(kSessionHeader);

    if (head.method == "DELETE") {
        if (session_id.empty()) {
            detail::HttpEndpoint::write_simple(fd, 400, "Bad Request",
                                               "Mcp-Session-Id required");
            return;
        }
        if (!options_.allow_client_termination) {
            detail::HttpEndpoint::write_simple(fd, 405, "Method Not Allowed",
                                               "session termination disabled");
            return;
        }
        auto entry = registry_->remove(session_id);
        if (!entry) {
            detail::HttpEndpoint::write_simple(fd, 404, "Not Found",
                                               "unknown session");
            return;
        }
        entry->transport->disconnect();
        if (on_session_closed_ && entry->session) {
            on_session_closed_(*entry->session);
        }
        MCP_LOG(info, "session terminated by client: " << id_prefix(session_id)
                                                       << " ("
                                                       << registry_->size()
                                                       << " active)");
        detail::HttpEndpoint::write_simple(fd, 200, "OK");
        return;
    }

    if (head.method == "GET") {
        if (!head.header_contains("accept", "text/event-stream")) {
            detail::HttpEndpoint::write_simple(fd, 406, "Not Acceptable",
                                               "expected text/event-stream");
            return;
        }
        if (session_id.empty()) {
            detail::HttpEndpoint::write_simple(fd, 400, "Bad Request",
                                               "Mcp-Session-Id required");
            return;
        }
        auto entry = registry_->find(session_id);
        if (!entry) {
            detail::HttpEndpoint::write_simple(fd, 404, "Not Found",
                                               "unknown session");
            return;
        }
        // Blocks for the stream's lifetime; per-session replay space
        // (FR-TRAN-007/009).
        MCP_LOG(debug, "SSE stream attach: session "
                           << id_prefix(session_id)
                           << (head.header("last-event-id").empty()
                                   ? ""
                                   : ", resume after event " +
                                         head.header("last-event-id")));
        entry->transport->channel().attach_stream(
            fd, head.header("last-event-id"), kProtocolVersion);
        return;
    }

    if (head.method != "POST") {
        detail::HttpEndpoint::write_simple(fd, 405, "Method Not Allowed",
                                           "use POST, GET, or DELETE");
        return;
    }

    auto frame = parse_frame(body);
    if (!frame) {
        write_jsonrpc_error(fd, 400, "Bad Request", frame.error());
        return;
    }

    detail::SessionRegistry::EntryPtr entry;
    detail::HeaderList extra_headers;
    if (session_id.empty()) {
        // Only initialize may open a new session; everything else must
        // carry the server-assigned id.
        if (!frame_contains_initialize(frame.value())) {
            write_jsonrpc_error(
                fd, 400, "Bad Request",
                Error(ErrorCode::InvalidRequest,
                      "Mcp-Session-Id header required (initialize first)"));
            return;
        }
        auto new_entry = std::make_shared<detail::SessionRegistry::Entry>();
        new_entry->transport = std::make_shared<detail::SessionTransport>(
            options_.http.replay_buffer_size, options_.http.sse_retry_ms);
        new_entry->session = factory_(new_entry->transport);
        if (!new_entry->session) {
            detail::HttpEndpoint::write_simple(fd, 500, "Internal Server Error",
                                               "session factory failed");
            return;
        }
        new_entry->session->connect();
        new_entry->last_activity = std::chrono::steady_clock::now();
        const std::string id = detail::generate_session_id();
        entry = registry_->create(id, new_entry);
        MCP_LOG(info, "session created: " << id_prefix(id) << " ("
                                          << registry_->size() << " active)");
        extra_headers.emplace_back("Mcp-Session-Id", id);
    } else {
        entry = registry_->find(session_id);
        if (!entry) {
            // Expired/unknown: 404 tells the client to re-initialize.
            detail::HttpEndpoint::write_simple(fd, 404, "Not Found",
                                               "unknown or expired session");
            return;
        }
    }

    detail::PostCapture capture;
    for (const auto& item_error : frame.value().item_errors) {
        capture.add_error_response(item_error);
    }
    for (auto& message : frame.value().messages) {
        entry->transport->deliver(std::move(message));
    }

    if (capture.empty()) {
        detail::HttpEndpoint::write_simple(fd, 202, "Accepted", "",
                                           "text/plain", extra_headers);
        return;
    }
    detail::HttpEndpoint::write_simple(fd, 200, "OK",
                                       capture.body(frame.value().was_batch),
                                       "application/json", extra_headers);
}

}  // namespace mcp
