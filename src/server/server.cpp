#include <mcp/server/server.hpp>

#include <algorithm>
#include <future>
#include <vector>

#include <mcp/methods.hpp>

namespace mcp {

namespace {

Result<std::optional<std::string>> parse_cursor(
    const std::optional<json>& params) {
    if (!params || !params->contains("cursor")) {
        return std::optional<std::string>{};
    }
    const auto& cursor = params->at("cursor");
    if (!cursor.is_string()) {
        return Error(ErrorCode::PaginationError, "cursor must be a string");
    }
    return std::optional<std::string>{cursor.get<std::string>()};
}

Result<std::string> require_string(const std::optional<json>& params,
                                   const char* key) {
    if (!params || !params->contains(key) || !params->at(key).is_string()) {
        return Error(ErrorCode::InvalidParams,
                     std::string("missing required string param '") + key + "'");
    }
    return params->at(key).get<std::string>();
}

template <typename T>
json page_to_json(const detail::Page<T>& page, const char* items_key) {
    json j{{items_key, page.items}};
    detail::set_optional(j, "nextCursor", page.next_cursor);
    return j;
}

}  // namespace

json CompleteResult::to_json() const {
    // At most 100 values go on the wire (FR-SRV-023 / spec).
    json vals = json::array();
    const std::size_t limit = std::min<std::size_t>(values.size(), 100);
    for (std::size_t i = 0; i < limit; ++i) {
        vals.push_back(values[i]);
    }
    json completion{{"values", vals}};
    detail::set_optional(completion, "total", total);
    detail::set_optional(completion, "hasMore", has_more);
    return json{{"completion", completion}};
}

Server::Server(std::string name, std::string version)
    : info_{std::move(name), std::nullopt, std::move(version)} {}

Result<void> Server::register_tool(const std::string& name, ToolSpec spec) {
    Tool tool;
    tool.name = name;
    tool.title = std::move(spec.title);
    tool.description = std::move(spec.description);
    tool.input_schema = std::move(spec.input_schema);
    tool.output_schema = std::move(spec.output_schema);
    return tools_.register_tool(std::move(tool), std::move(spec.handler));
}

void Server::set_instructions(std::string instructions) {
    instructions_ = std::move(instructions);
}

ServerCapabilities Server::capabilities() const {
    ServerCapabilities caps;
    if (tools_.size() > 0) {
        caps.tools = ToolsCapability{true};
    }
    if (resources_.resource_count() > 0 || resources_.template_count() > 0) {
        caps.resources = ResourcesCapability{true, true};
    }
    if (prompts_.size() > 0) {
        caps.prompts = PromptsCapability{true};
    }
    caps.logging = LoggingCapability{};
    if (prompts_.has_completions() || resources_.has_completions()) {
        caps.completions = CompletionsCapability{};
    }
    return caps;
}

ServerOptions Server::server_options() const {
    ServerOptions options;
    options.server_info = info_;
    options.capabilities = capabilities();
    options.instructions = instructions_;
    options.allow_reinitialize = allow_reinitialize_;
    return options;
}

void Server::attach(ServerSession& session) {
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        if (std::find(sessions_.begin(), sessions_.end(), &session) ==
            sessions_.end()) {
            sessions_.push_back(&session);
        }
    }
    auto& router = session.router();

    // --- Tools (FR-SRV-004/005) ---
    router.set_request_handler(
        methods::kToolsList,
        [this](const std::optional<json>& params) -> Result<json> {
            auto cursor = parse_cursor(params);
            if (!cursor) return std::move(cursor.error());
            auto page = tools_.list_tools(cursor.value());
            if (!page) return std::move(page.error());
            return page_to_json(page.value(), "tools");
        });
    router.set_request_handler(
        methods::kToolsCall,
        [this](const std::optional<json>& params) -> Result<json> {
            auto name = require_string(params, "name");
            if (!name) return std::move(name.error());
            const json arguments =
                params->contains("arguments") ? params->at("arguments")
                                              : json::object();
            auto result = tools_.call_tool(name.value(), arguments);
            if (!result) return std::move(result.error());
            return result.value().to_json();
        });

    // --- Resources (FR-SRV-011..013) ---
    router.set_request_handler(
        methods::kResourcesList,
        [this](const std::optional<json>& params) -> Result<json> {
            auto cursor = parse_cursor(params);
            if (!cursor) return std::move(cursor.error());
            auto page = resources_.list_resources(cursor.value());
            if (!page) return std::move(page.error());
            return page_to_json(page.value(), "resources");
        });
    router.set_request_handler(
        methods::kResourcesTemplatesList,
        [this](const std::optional<json>& params) -> Result<json> {
            auto cursor = parse_cursor(params);
            if (!cursor) return std::move(cursor.error());
            auto page = resources_.list_resource_templates(cursor.value());
            if (!page) return std::move(page.error());
            return page_to_json(page.value(), "resourceTemplates");
        });
    router.set_request_handler(
        methods::kResourcesRead,
        [this](const std::optional<json>& params) -> Result<json> {
            auto uri = require_string(params, "uri");
            if (!uri) return std::move(uri.error());
            auto result = resources_.read_resource(uri.value());
            if (!result) return std::move(result.error());
            return result.value().to_json();
        });
    router.set_request_handler(
        methods::kResourcesSubscribe,
        [this](const std::optional<json>& params) -> Result<json> {
            auto uri = require_string(params, "uri");
            if (!uri) return std::move(uri.error());
            auto result = resources_.subscribe(uri.value());
            if (!result) return std::move(result.error());
            return json::object();
        });
    router.set_request_handler(
        methods::kResourcesUnsubscribe,
        [this](const std::optional<json>& params) -> Result<json> {
            auto uri = require_string(params, "uri");
            if (!uri) return std::move(uri.error());
            resources_.unsubscribe(uri.value());
            return json::object();
        });

    // --- Prompts (FR-SRV-017) ---
    router.set_request_handler(
        methods::kPromptsList,
        [this](const std::optional<json>& params) -> Result<json> {
            auto cursor = parse_cursor(params);
            if (!cursor) return std::move(cursor.error());
            auto page = prompts_.list_prompts(cursor.value());
            if (!page) return std::move(page.error());
            return page_to_json(page.value(), "prompts");
        });
    router.set_request_handler(
        methods::kPromptsGet,
        [this](const std::optional<json>& params) -> Result<json> {
            auto name = require_string(params, "name");
            if (!name) return std::move(name.error());
            const json arguments =
                params->contains("arguments") ? params->at("arguments")
                                              : json::object();
            auto result = prompts_.get_prompt(name.value(), arguments);
            if (!result) return std::move(result.error());
            return result.value().to_json();
        });

    // --- Logging (FR-SRV-020) ---
    router.set_request_handler(
        methods::kLoggingSetLevel,
        [this](const std::optional<json>& params) -> Result<json> {
            auto level_name = require_string(params, "level");
            if (!level_name) return std::move(level_name.error());
            const auto level = logging_level_from_string(level_name.value());
            if (!level) {
                return Error(ErrorCode::InvalidParams,
                             "unknown logging level: " + level_name.value());
            }
            logger_.set_level(*level);
            return json::object();
        });

    // --- Completion (FR-SRV-023) ---
    router.set_request_handler(
        methods::kCompletionComplete,
        [this](const std::optional<json>& params) -> Result<json> {
            if (!params || !params->contains("ref") ||
                !params->contains("argument")) {
                return Error(ErrorCode::InvalidParams,
                             "completion/complete requires ref and argument");
            }
            const auto& ref = params->at("ref");
            const auto& argument = params->at("argument");
            if (!ref.is_object() || !argument.is_object() ||
                !ref.contains("type") || !argument.contains("name") ||
                !argument.contains("value")) {
                return Error(ErrorCode::InvalidParams,
                             "malformed completion reference or argument");
            }
            const auto type = ref.at("type").get<std::string>();
            const auto arg_name = argument.at("name").get<std::string>();
            const auto arg_value = argument.at("value").get<std::string>();

            std::optional<CompleteResult> result;
            if (type == "ref/prompt" && ref.contains("name")) {
                result = prompts_.complete(ref.at("name").get<std::string>(),
                                           arg_name, arg_value);
            } else if (type == "ref/resource" && ref.contains("uri")) {
                result = resources_.complete(ref.at("uri").get<std::string>(),
                                             arg_name, arg_value);
            } else {
                return Error(ErrorCode::InvalidParams,
                             "unknown completion reference type: " + type);
            }
            return result.value_or(CompleteResult{}).to_json();
        });

    // --- list_changed + logging notifications (FR-SRV-006/013/018/021) ---
    tools_.set_changed_callback([this] {
        send_notification_if_operating(methods::kNotificationToolsListChanged,
                                       std::nullopt);
    });
    resources_.set_changed_callback([this] {
        send_notification_if_operating(
            methods::kNotificationResourcesListChanged, std::nullopt);
    });
    prompts_.set_changed_callback([this] {
        send_notification_if_operating(methods::kNotificationPromptsListChanged,
                                       std::nullopt);
    });
    logger_.set_emitter([this](const json& params) {
        send_notification_if_operating(methods::kNotificationMessage, params);
    });
}

void Server::detach(ServerSession& session) {
    std::lock_guard<std::mutex> lock(session_mutex_);
    sessions_.erase(
        std::remove(sessions_.begin(), sessions_.end(), &session),
        sessions_.end());
}

int Server::run(std::shared_ptr<Transport> transport) {
    ServerSession session(std::move(transport), server_options());
    attach(session);

    std::promise<void> closed;
    session.set_close_callback([&closed] { closed.set_value(); });
    session.connect();
    closed.get_future().wait();

    detach(session);
    return 0;
}

ServerSession* Server::session() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return sessions_.size() == 1 ? sessions_.front() : nullptr;
}

std::vector<ServerSession*> Server::sessions() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return sessions_;
}

void Server::notify_resource_updated(const std::string& uri) {
    if (resources_.is_subscribed(uri)) {
        send_notification_if_operating(methods::kNotificationResourcesUpdated,
                                       json{{"uri", uri}});
    }
}

void Server::send_notification_if_operating(const std::string& method,
                                            std::optional<json> params) {
    // Broadcast to every attached Operating session (multi-session HTTP).
    std::lock_guard<std::mutex> lock(session_mutex_);
    for (ServerSession* session : sessions_) {
        if (session->state() == SessionState::Operating) {
            session->send_notification(method, params);
        }
    }
}

}  // namespace mcp
