#include <mcp/client/client.hpp>

#include <mcp/detail/schema.hpp>
#include <mcp/methods.hpp>

namespace mcp {

namespace {

/// Runs a typed parse, mapping json exceptions to a Result error.
template <typename F>
auto parse_response(F&& parse) -> decltype(parse()) {
#if defined(__cpp_exceptions)
    try {
        return parse();
    } catch (const json::exception& e) {
        return Error(ErrorCode::InternalError,
                     std::string("malformed server response: ") + e.what());
    }
#else
    return parse();
#endif
}

std::optional<json> cursor_params(const std::optional<std::string>& cursor) {
    if (!cursor) {
        return std::nullopt;
    }
    return json{{"cursor", *cursor}};
}

template <typename T>
Result<Client::ListResult<T>> parse_list(const json& j, const char* key) {
    return parse_response([&]() -> Result<Client::ListResult<T>> {
        Client::ListResult<T> result;
        result.items = j.at(key).template get<std::vector<T>>();
        detail::get_optional(j, "nextCursor", result.next_cursor);
        return result;
    });
}

}  // namespace

Client::Client(std::string name, std::string version)
    : info_{std::move(name), std::nullopt, std::move(version)} {}

Client::~Client() { disconnect(); }

void Client::set_sampling_handler(SamplingHandler handler) {
    sampling_handler_ = std::move(handler);
}

void Client::set_elicitation_handler(ElicitationHandler handler) {
    elicitation_handler_ = std::move(handler);
}

void Client::on_tools_list_changed(std::function<void()> callback) {
    tools_changed_ = std::move(callback);
}
void Client::on_resources_list_changed(std::function<void()> callback) {
    resources_changed_ = std::move(callback);
}
void Client::on_prompts_list_changed(std::function<void()> callback) {
    prompts_changed_ = std::move(callback);
}
void Client::on_resource_updated(
    std::function<void(const std::string&)> callback) {
    resource_updated_ = std::move(callback);
}
void Client::on_log_message(std::function<void(const json&)> callback) {
    log_message_ = std::move(callback);
}

ClientCapabilities Client::capabilities() const {
    ClientCapabilities caps;
    if (roots_enabled_ || roots_.size() > 0) {
        caps.roots = RootsCapability{true};
    }
    if (sampling_handler_) {
        caps.sampling = SamplingCapability{true, true};
    }
    if (elicitation_handler_) {
        caps.elicitation = ElicitationCapability{true, true};
    }
    return caps;
}

ClientOptions Client::client_options() const {
    ClientOptions options;
    options.client_info = info_;
    options.capabilities = capabilities();
    return options;
}

Result<InitializeResult> Client::connect(std::shared_ptr<Transport> transport) {
    if (session_) {
        return Error(ErrorCode::InvalidRequest, "client is already connected");
    }
    transport_ = std::move(transport);
    session_ = std::make_unique<ClientSession>(transport_, client_options());
    wire_session();

    session_->connect();
    auto init = session_->initialize();
    if (!init) {
        disconnect();
        session_.reset();
    }
    return init;
}

void Client::wire_session() {
    auto& router = session_->router();

    // roots/list (FR-CLI-004)
    router.set_request_handler(
        methods::kRootsList,
        [this](const std::optional<json>&) -> Result<json> {
            return json{{"roots", roots_.list_roots()}};
        });
    roots_.set_changed_callback([this] {
        // FR-CLI-005
        if (session_ && session_->state() == SessionState::Operating) {
            session_->send_notification(methods::kNotificationRootsListChanged);
        }
    });

    // sampling/createMessage (FR-CLI-001)
    router.set_request_handler(
        methods::kSamplingCreateMessage,
        [this](const std::optional<json>& params) -> Result<json> {
            if (!sampling_handler_) {
                return Error(ErrorCode::CapabilityNotSupported,
                             "no sampling handler configured");
            }
            if (!params) {
                return Error(ErrorCode::InvalidParams,
                             "createMessage requires params");
            }
            return parse_response([&]() -> Result<json> {
                const auto request = params->get<CreateMessageParams>();
                auto result = sampling_handler_(request);
                if (!result) {
                    return std::move(result.error());
                }
                return result.value().to_json();
            });
        });

    // elicitation/create (FR-CLI-006)
    router.set_request_handler(
        methods::kElicitationCreate,
        [this](const std::optional<json>& params) -> Result<json> {
            if (!elicitation_handler_) {
                return Error(ErrorCode::CapabilityNotSupported,
                             "no elicitation handler configured");
            }
            if (!params) {
                return Error(ErrorCode::InvalidParams,
                             "elicitation requires params");
            }
            auto request = ElicitRequest::from_json(*params);
            if (!request) {
                return std::move(request.error());
            }
            auto result = elicitation_handler_(request.value());
            if (!result) {
                return std::move(result.error());
            }
            // Accepted form content must satisfy the requested schema.
            if (result.value().action == ElicitAction::Accept &&
                request.value().requested_schema && result.value().content) {
                if (auto valid = detail::validate_schema(
                        *result.value().content,
                        *request.value().requested_schema);
                    !valid) {
                    return Error(ErrorCode::InvalidParams,
                                 "elicitation content does not match the "
                                 "requested schema: " +
                                     valid.error().message);
                }
            }
            return result.value().to_json();
        });

    // Server-side notifications -> user callbacks.
    if (tools_changed_) {
        router.set_notification_handler(
            methods::kNotificationToolsListChanged,
            [this](const std::optional<json>&) { tools_changed_(); });
    }
    if (resources_changed_) {
        router.set_notification_handler(
            methods::kNotificationResourcesListChanged,
            [this](const std::optional<json>&) { resources_changed_(); });
    }
    if (prompts_changed_) {
        router.set_notification_handler(
            methods::kNotificationPromptsListChanged,
            [this](const std::optional<json>&) { prompts_changed_(); });
    }
    if (resource_updated_) {
        router.set_notification_handler(
            methods::kNotificationResourcesUpdated,
            [this](const std::optional<json>& params) {
                if (params && params->contains("uri") &&
                    params->at("uri").is_string()) {
                    resource_updated_(params->at("uri").get<std::string>());
                }
            });
    }
    if (log_message_) {
        router.set_notification_handler(
            methods::kNotificationMessage,
            [this](const std::optional<json>& params) {
                if (params) {
                    log_message_(*params);
                }
            });
    }
}

void Client::disconnect() {
    if (session_) {
        session_->disconnect();
    }
}

std::optional<ServerCapabilities> Client::server_capabilities() const {
    return session_ ? session_->server_capabilities() : std::nullopt;
}

Result<json> Client::request(const char* method, std::optional<json> params) {
    if (!session_) {
        return Error(ErrorCode::ConnectionNotInitialized,
                     "client is not connected");
    }
    Session::RequestOptions options;
    options.timeout = timeout_;
    return session_->send_request_sync(method, std::move(params), options);
}

Result<json> Client::ping() { return request(methods::kPing, std::nullopt); }

Result<Client::ListResult<Tool>> Client::list_tools(
    std::optional<std::string> cursor) {
    auto response = request(methods::kToolsList, cursor_params(cursor));
    if (!response) return std::move(response.error());
    return parse_list<Tool>(response.value(), "tools");
}

Result<CallToolResult> Client::call_tool(const std::string& name,
                                         json arguments) {
    auto response = request(
        methods::kToolsCall,
        json{{"name", name}, {"arguments", std::move(arguments)}});
    if (!response) return std::move(response.error());
    return parse_response([&]() -> Result<CallToolResult> {
        auto content = content_list_from_json(
            response.value().value("content", json::array()));
        if (!content) {
            return std::move(content.error());
        }
        return CallToolResult{std::move(content).value(),
                              response.value().value("isError", false)};
    });
}

Result<Client::ListResult<Resource>> Client::list_resources(
    std::optional<std::string> cursor) {
    auto response = request(methods::kResourcesList, cursor_params(cursor));
    if (!response) return std::move(response.error());
    return parse_list<Resource>(response.value(), "resources");
}

Result<Client::ListResult<ResourceTemplate>> Client::list_resource_templates(
    std::optional<std::string> cursor) {
    auto response =
        request(methods::kResourcesTemplatesList, cursor_params(cursor));
    if (!response) return std::move(response.error());
    return parse_list<ResourceTemplate>(response.value(), "resourceTemplates");
}

Result<ReadResourceResult> Client::read_resource(const std::string& uri) {
    auto response = request(methods::kResourcesRead, json{{"uri", uri}});
    if (!response) return std::move(response.error());
    return parse_response([&]() -> Result<ReadResourceResult> {
        return ReadResourceResult{
            response.value().at("contents").get<std::vector<ResourceContents>>()};
    });
}

Result<void> Client::subscribe_resource(const std::string& uri) {
    auto response = request(methods::kResourcesSubscribe, json{{"uri", uri}});
    if (!response) return std::move(response.error());
    return Result<void>::ok();
}

Result<void> Client::unsubscribe_resource(const std::string& uri) {
    auto response = request(methods::kResourcesUnsubscribe, json{{"uri", uri}});
    if (!response) return std::move(response.error());
    return Result<void>::ok();
}

Result<Client::ListResult<Prompt>> Client::list_prompts(
    std::optional<std::string> cursor) {
    auto response = request(methods::kPromptsList, cursor_params(cursor));
    if (!response) return std::move(response.error());
    return parse_list<Prompt>(response.value(), "prompts");
}

Result<GetPromptResult> Client::get_prompt(const std::string& name,
                                           json arguments) {
    auto response = request(
        methods::kPromptsGet,
        json{{"name", name}, {"arguments", std::move(arguments)}});
    if (!response) return std::move(response.error());
    return parse_response([&]() -> Result<GetPromptResult> {
        GetPromptResult result;
        detail::get_optional(response.value(), "description",
                             result.description);
        for (const auto& message :
             response.value().value("messages", json::array())) {
            auto content = content_from_json(message.at("content"));
            if (!content) {
                return std::move(content.error());
            }
            const auto role =
                role_from_string(message.value("role", "user"));
            result.messages.push_back(PromptMessage{
                role.value_or(Role::User), std::move(content).value()});
        }
        return result;
    });
}

namespace {

Result<CompleteResult> parse_completion(const json& j) {
    return parse_response([&]() -> Result<CompleteResult> {
        const auto& completion = j.at("completion");
        CompleteResult result;
        result.values = completion.at("values").get<std::vector<std::string>>();
        detail::get_optional(completion, "total", result.total);
        detail::get_optional(completion, "hasMore", result.has_more);
        return result;
    });
}

}  // namespace

Result<CompleteResult> Client::complete_prompt(const std::string& prompt_name,
                                               const std::string& argument,
                                               const std::string& value) {
    auto response = request(
        methods::kCompletionComplete,
        json{{"ref", {{"type", "ref/prompt"}, {"name", prompt_name}}},
             {"argument", {{"name", argument}, {"value", value}}}});
    if (!response) return std::move(response.error());
    return parse_completion(response.value());
}

Result<CompleteResult> Client::complete_resource(
    const std::string& uri_template, const std::string& argument,
    const std::string& value) {
    auto response = request(
        methods::kCompletionComplete,
        json{{"ref", {{"type", "ref/resource"}, {"uri", uri_template}}},
             {"argument", {{"name", argument}, {"value", value}}}});
    if (!response) return std::move(response.error());
    return parse_completion(response.value());
}

Result<void> Client::set_logging_level(LoggingLevel level) {
    auto response =
        request(methods::kLoggingSetLevel,
                json{{"level", logging_level_to_string(level)}});
    if (!response) return std::move(response.error());
    return Result<void>::ok();
}

}  // namespace mcp
