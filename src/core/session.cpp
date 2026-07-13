#define MCP_LOG_COMPONENT "session"

#include <mcp/core/session.hpp>

#include <algorithm>
#include <mcp/sys/threading.hpp>

#include <mcp/log.hpp>
#include <mcp/methods.hpp>

namespace mcp {

namespace {

constexpr std::size_t kMaxTrackedCancellations = 1024;

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

}  // namespace

// ---------------------------------------------------------------- Session

Session::Session(std::shared_ptr<Transport> transport)
    : transport_(std::move(transport)) {
    router_.set_request_handler(methods::kPing,
                                [](const std::optional<json>&) -> Result<json> {
                                    return json::object();
                                });
    router_.set_notification_handler(
        methods::kNotificationProgress,
        [this](const std::optional<json>& params) { handle_progress(params); });
    router_.set_notification_handler(
        methods::kNotificationCancelled,
        [this](const std::optional<json>& params) { handle_cancelled(params); });

    transport_->set_message_handler(
        [this](Message message) { handle_message(std::move(message)); });
    transport_->set_error_handler(
        [this](Error error) { on_transport_error(error); });
    transport_->set_close_handler([this] { handle_close(); });

    timer_thread_ = mcp::sys::thread([this] { timer_loop(); });
}

Session::~Session() {
#if defined(__cpp_exceptions)
    try {
        transport_->disconnect();
    } catch (...) {
    }
#else
    transport_->disconnect();
#endif
    {
        std::lock_guard<mcp::sys::mutex> lock(timer_mutex_);
        stop_timer_ = true;
    }
    timer_cv_.notify_all();
    if (timer_thread_.joinable()) {
        timer_thread_.join();
    }
}

void Session::connect() { transport_->connect(); }

void Session::disconnect() {
    set_state(SessionState::Shutdown);
    transport_->disconnect();
}

void Session::send_message(const Message& message) { transport_->send(message); }

RequestId Session::send_request(const std::string& method,
                                std::optional<json> params,
                                MessageRouter::ResponseCallback callback,
                                RequestOptions options) {
    RequestId id{next_request_id_.fetch_add(1)};

    if (options.progress_token) {
        json p = params ? std::move(*params) : json::object();
        p["_meta"]["progressToken"] = request_id_to_json(*options.progress_token);
        params = std::move(p);
    }

    router_.register_pending(id, std::move(callback));

    const bool timed = options.timeout && options.timeout->count() > 0;
    if (timed || (options.progress_token && options.on_progress)) {
        std::lock_guard<mcp::sys::mutex> lock(timer_mutex_);
        if (timed) {
            deadlines_[id] = Deadline{
                std::chrono::steady_clock::now() + *options.timeout,
                *options.timeout, options.progress_token};
        }
        if (options.progress_token && options.on_progress) {
            progress_callbacks_[*options.progress_token] =
                std::move(options.on_progress);
        }
    }
    timer_cv_.notify_all();

    send_message(Message(JsonRpcRequest{id, method, std::move(params)}));
    return id;
}

Result<json> Session::send_request_sync(const std::string& method,
                                        std::optional<json> params,
                                        RequestOptions options) {
    auto slot = std::make_shared<mcp::sys::OneShot<Result<json>>>();
    send_request(
        method, std::move(params),
        [slot](Result<json> result) { slot->set(std::move(result)); },
        std::move(options));
    return slot->get();
}

void Session::send_notification(const std::string& method,
                                std::optional<json> params) {
    send_message(Message(JsonRpcNotification{method, std::move(params)}));
}

void Session::cancel_request(const RequestId& id,
                             std::optional<std::string> reason) {
    clear_tracking(id);
    auto callback = router_.take_pending(id);
    if (!callback) {
        return;  // Already completed: nothing to cancel (FR-CORE-015).
    }
    CancelledNotification note{id, std::move(reason)};
    send_notification(methods::kNotificationCancelled, note.to_json());
    (*callback)(Result<json>::err(
        Error(ErrorCode::InternalError, "request was cancelled")));
}

void Session::send_progress(const ProgressToken& token, double progress,
                            std::optional<double> total,
                            std::optional<std::string> message) {
    ProgressNotification note{token, progress, total, std::move(message)};
    send_notification(methods::kNotificationProgress, note.to_json());
}

bool Session::is_cancelled(const RequestId& id) const {
    std::lock_guard<mcp::sys::mutex> lock(cancelled_mutex_);
    return cancelled_incoming_.count(id) > 0;
}

void Session::set_close_callback(std::function<void()> callback) {
    std::lock_guard<mcp::sys::mutex> lock(callback_mutex_);
    close_callback_ = std::move(callback);
}

void Session::set_error_callback(std::function<void(const Error&)> callback) {
    std::lock_guard<mcp::sys::mutex> lock(callback_mutex_);
    error_callback_ = std::move(callback);
}

std::optional<Error> Session::check_incoming_request(const JsonRpcRequest&) {
    return std::nullopt;
}

void Session::on_transport_error(const Error& error) {
    std::function<void(const Error&)> callback;
    {
        std::lock_guard<mcp::sys::mutex> lock(callback_mutex_);
        callback = error_callback_;
    }
    if (callback) {
        callback(error);
    }
}

void Session::handle_message(Message message) {
    if (const auto* request = std::get_if<JsonRpcRequest>(&message)) {
        if (request->method == methods::kInitialize) {
            std::lock_guard<mcp::sys::mutex> lock(cancelled_mutex_);
            protected_request_id_ = request->id;
        }
        MCP_LOG(debug, "--> " << request->method << " (id "
                              << request_id_to_string(request->id) << ")");
        if (auto error = check_incoming_request(*request)) {
            MCP_LOG(warn, "<-- error " << error->code << " for "
                                       << request->method << ": "
                                       << error->message);
            JsonRpcResponse response;
            response.id = request->id;
            response.error = std::move(*error);
            send_message(Message(std::move(response)));
            return;
        }
        if (auto response = router_.dispatch(message)) {
            if (response->is_error()) {
                MCP_LOG(warn, "<-- error " << response->error->code << " for "
                                           << request->method << ": "
                                           << response->error->message);
            } else {
                MCP_LOG(debug, "<-- result (id "
                                   << request_id_to_string(request->id) << ")");
            }
            send_message(Message(std::move(*response)));
        }
        return;
    }

    if (const auto* response = std::get_if<JsonRpcResponse>(&message)) {
        if (response->id) {
            clear_tracking(*response->id);
        }
    }
    router_.dispatch(message);
}

void Session::handle_close() {
    set_state(SessionState::Shutdown);
    {
        std::lock_guard<mcp::sys::mutex> lock(timer_mutex_);
        deadlines_.clear();
        progress_callbacks_.clear();
    }
    router_.fail_all_pending(
        Error(ErrorCode::InternalError, "connection closed"));

    std::function<void()> callback;
    {
        std::lock_guard<mcp::sys::mutex> lock(callback_mutex_);
        callback = close_callback_;
    }
    if (callback) {
        callback();
    }
}

void Session::handle_progress(const std::optional<json>& params) {
    if (!params) {
        return;
    }
    auto parsed = ProgressNotification::from_json(*params);
    if (!parsed) {
        return;
    }
    const auto& note = parsed.value();

    std::function<void(const ProgressNotification&)> callback;
    {
        std::lock_guard<mcp::sys::mutex> lock(timer_mutex_);
        for (auto& [id, deadline] : deadlines_) {
            if (deadline.progress_token &&
                *deadline.progress_token == note.progress_token) {
                // Progress resets the timeout clock (FR-CORE-004/014).
                deadline.at = std::chrono::steady_clock::now() + deadline.timeout;
            }
        }
        if (auto it = progress_callbacks_.find(note.progress_token);
            it != progress_callbacks_.end()) {
            callback = it->second;
        }
    }
    timer_cv_.notify_all();
    if (callback) {
        callback(note);
    }
}

void Session::handle_cancelled(const std::optional<json>& params) {
    if (!params) {
        return;
    }
    auto parsed = CancelledNotification::from_json(*params);
    if (!parsed) {
        return;
    }
    std::lock_guard<mcp::sys::mutex> lock(cancelled_mutex_);
    if (protected_request_id_ && parsed.value().request_id == *protected_request_id_) {
        return;  // initialize MUST NOT be cancelled (FR-CORE-015).
    }
    if (cancelled_incoming_.size() >= kMaxTrackedCancellations) {
        cancelled_incoming_.clear();
    }
    cancelled_incoming_.insert(parsed.value().request_id);
}

void Session::clear_tracking(const RequestId& id) {
    std::lock_guard<mcp::sys::mutex> lock(timer_mutex_);
    if (auto it = deadlines_.find(id); it != deadlines_.end()) {
        if (it->second.progress_token) {
            progress_callbacks_.erase(*it->second.progress_token);
        }
        deadlines_.erase(it);
    }
    timer_cv_.notify_all();
}

void Session::timer_loop() {
    std::unique_lock<mcp::sys::mutex> lock(timer_mutex_);
    while (!stop_timer_) {
        if (deadlines_.empty()) {
            timer_cv_.wait(lock);
            continue;
        }
        auto nearest = std::min_element(
                           deadlines_.begin(), deadlines_.end(),
                           [](const auto& a, const auto& b) {
                               return a.second.at < b.second.at;
                           })
                           ->second.at;
        timer_cv_.wait_until(lock, nearest);
        if (stop_timer_) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        std::vector<RequestId> expired;
        for (auto it = deadlines_.begin(); it != deadlines_.end();) {
            if (it->second.at <= now) {
                expired.push_back(it->first);
                if (it->second.progress_token) {
                    progress_callbacks_.erase(*it->second.progress_token);
                }
                it = deadlines_.erase(it);
            } else {
                ++it;
            }
        }

        lock.unlock();
        for (const auto& id : expired) {
            // Per spec, the requester issues a cancellation on timeout.
            CancelledNotification note{id, std::string("request timed out")};
            send_notification(methods::kNotificationCancelled, note.to_json());
            router_.fail_pending(
                id, Error(ErrorCode::InternalError, "request timed out"));
        }
        lock.lock();
    }
}

// ---------------------------------------------------------- ServerSession

ServerSession::ServerSession(std::shared_ptr<Transport> transport,
                             ServerOptions options)
    : Session(std::move(transport)), options_(std::move(options)) {
    router_.set_request_handler(
        methods::kInitialize,
        [this](const std::optional<json>& params) -> Result<json> {
            if (state() != SessionState::Uninitialized) {
                if (!options_.allow_reinitialize) {
                    return Error(ErrorCode::InvalidRequest,
                                 "initialize already received");
                }
                // Sessionless-HTTP mode: a fresh initialize starts a new
                // logical session on this transport.
                std::lock_guard<mcp::sys::mutex> lock(peer_mutex_);
                client_info_.reset();
                client_capabilities_.reset();
            }
            if (!params) {
                return Error(ErrorCode::InvalidParams,
                             "initialize requires params");
            }
            InitializeParams parsed;
#if defined(__cpp_exceptions)
            try {
                parsed = params->get<InitializeParams>();
            } catch (const json::exception& e) {
                return Error(ErrorCode::InvalidParams,
                             std::string("invalid initialize params: ") + e.what());
            }
#else
            parsed = params->get<InitializeParams>();
#endif
            {
                std::lock_guard<mcp::sys::mutex> lock(peer_mutex_);
                client_info_ = parsed.client_info;
                client_capabilities_ = parsed.capabilities;
            }
            set_state(SessionState::Initializing);

            // If the requested version is supported, echo it; otherwise
            // offer our preferred version (FR-CORE-006).
            std::string negotiated = options_.supported_versions.front();
            for (const auto& v : options_.supported_versions) {
                if (v == parsed.protocol_version) {
                    negotiated = v;
                    break;
                }
            }
            MCP_LOG(info, "initialize: client \""
                              << parsed.client_info.name << "\" v"
                              << parsed.client_info.version << " -> protocol "
                              << negotiated);
            return json(InitializeResult{negotiated, options_.capabilities,
                                         options_.server_info,
                                         options_.instructions});
        });

    router_.set_notification_handler(
        methods::kNotificationInitialized, [this](const std::optional<json>&) {
            if (state() == SessionState::Initializing) {
                set_state(SessionState::Operating);
                MCP_LOG(info, "session operating");
            }
        });
}

std::optional<Implementation> ServerSession::client_info() const {
    std::lock_guard<mcp::sys::mutex> lock(peer_mutex_);
    return client_info_;
}

std::optional<ClientCapabilities> ServerSession::client_capabilities() const {
    std::lock_guard<mcp::sys::mutex> lock(peer_mutex_);
    return client_capabilities_;
}

std::optional<Error> ServerSession::check_incoming_request(
    const JsonRpcRequest& request) {
    if (request.method == methods::kInitialize ||
        request.method == methods::kPing) {
        return std::nullopt;
    }
    if (state() != SessionState::Operating) {
        return Error(ErrorCode::ConnectionNotInitialized,
                     "session is not initialized");
    }

    const struct {
        const char* prefix;
        bool declared;
    } gates[] = {
        {"tools/", options_.capabilities.tools.has_value()},
        {"resources/", options_.capabilities.resources.has_value()},
        {"prompts/", options_.capabilities.prompts.has_value()},
        {"logging/", options_.capabilities.logging.has_value()},
        {"completion/", options_.capabilities.completions.has_value()},
        {"tasks/", options_.capabilities.tasks.has_value()},
    };
    for (const auto& gate : gates) {
        if (starts_with(request.method, gate.prefix) && !gate.declared) {
            return Error(ErrorCode::CapabilityNotSupported,
                         "capability not supported: " + request.method);
        }
    }
    return std::nullopt;
}

void ServerSession::on_transport_error(const Error& error) {
    // Answer wire-level parse/validation failures with a null-id error
    // response, per JSON-RPC 2.0.
    if (error.code == static_cast<int>(ErrorCode::ParseError) ||
        error.code == static_cast<int>(ErrorCode::InvalidRequest)) {
        JsonRpcResponse response;
        response.error = error;
        send_message(Message(std::move(response)));
    }
    Session::on_transport_error(error);
}

// ---------------------------------------------------------- ClientSession

ClientSession::ClientSession(std::shared_ptr<Transport> transport,
                             ClientOptions options)
    : Session(std::move(transport)), options_(std::move(options)) {}

Result<InitializeResult> ClientSession::initialize() {
    if (state() != SessionState::Uninitialized) {
        return Error(ErrorCode::InvalidRequest, "session already initialized");
    }
    set_state(SessionState::Initializing);

    InitializeParams params{options_.supported_versions.front(),
                            options_.capabilities, options_.client_info};
    RequestOptions request_options;
    if (options_.initialize_timeout.count() > 0) {
        request_options.timeout = options_.initialize_timeout;
    }
    auto response =
        send_request_sync(methods::kInitialize, json(params), request_options);
    if (!response) {
        set_state(SessionState::Uninitialized);
        return response.error();
    }

    InitializeResult result;
#if defined(__cpp_exceptions)
    try {
        result = response.value().get<InitializeResult>();
    } catch (const json::exception& e) {
        set_state(SessionState::Uninitialized);
        return Error(ErrorCode::InternalError,
                     std::string("invalid initialize result: ") + e.what());
    }
#else
    result = response.value().get<InitializeResult>();
#endif

    const auto& versions = options_.supported_versions;
    if (std::find(versions.begin(), versions.end(), result.protocol_version) ==
        versions.end()) {
        // Client SHALL disconnect on an unsupported version (FR-CORE-006).
        disconnect();
        return Error(ErrorCode::InvalidRequest,
                     "unsupported protocol version: " + result.protocol_version);
    }

    {
        std::lock_guard<mcp::sys::mutex> lock(peer_mutex_);
        server_info_ = result.server_info;
        server_capabilities_ = result.capabilities;
    }
    send_notification(methods::kNotificationInitialized);
    set_state(SessionState::Operating);
    return result;
}

std::optional<Implementation> ClientSession::server_info() const {
    std::lock_guard<mcp::sys::mutex> lock(peer_mutex_);
    return server_info_;
}

std::optional<ServerCapabilities> ClientSession::server_capabilities() const {
    std::lock_guard<mcp::sys::mutex> lock(peer_mutex_);
    return server_capabilities_;
}

std::optional<Error> ClientSession::check_incoming_request(
    const JsonRpcRequest& request) {
    if (request.method == methods::kPing) {
        return std::nullopt;
    }
    if (state() != SessionState::Operating) {
        return Error(ErrorCode::ConnectionNotInitialized,
                     "session is not initialized");
    }

    const struct {
        const char* prefix;
        bool declared;
    } gates[] = {
        {"sampling/", options_.capabilities.sampling.has_value()},
        {"roots/", options_.capabilities.roots.has_value()},
        {"elicitation/", options_.capabilities.elicitation.has_value()},
        {"tasks/", options_.capabilities.tasks.has_value()},
    };
    for (const auto& gate : gates) {
        if (starts_with(request.method, gate.prefix) && !gate.declared) {
            return Error(ErrorCode::CapabilityNotSupported,
                         "capability not supported: " + request.method);
        }
    }
    return std::nullopt;
}

}  // namespace mcp
