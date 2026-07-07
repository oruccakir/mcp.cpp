#pragma once

namespace mcp::methods {

inline constexpr const char* kInitialize = "initialize";
inline constexpr const char* kPing = "ping";

inline constexpr const char* kToolsList = "tools/list";
inline constexpr const char* kToolsCall = "tools/call";

inline constexpr const char* kResourcesList = "resources/list";
inline constexpr const char* kResourcesRead = "resources/read";
inline constexpr const char* kResourcesTemplatesList = "resources/templates/list";
inline constexpr const char* kResourcesSubscribe = "resources/subscribe";
inline constexpr const char* kResourcesUnsubscribe = "resources/unsubscribe";

inline constexpr const char* kPromptsList = "prompts/list";
inline constexpr const char* kPromptsGet = "prompts/get";

inline constexpr const char* kCompletionComplete = "completion/complete";
inline constexpr const char* kLoggingSetLevel = "logging/setLevel";

inline constexpr const char* kSamplingCreateMessage = "sampling/createMessage";
inline constexpr const char* kRootsList = "roots/list";
inline constexpr const char* kElicitationCreate = "elicitation/create";

inline constexpr const char* kNotificationInitialized = "notifications/initialized";
inline constexpr const char* kNotificationCancelled = "notifications/cancelled";
inline constexpr const char* kNotificationProgress = "notifications/progress";
inline constexpr const char* kNotificationMessage = "notifications/message";
inline constexpr const char* kNotificationToolsListChanged =
    "notifications/tools/list_changed";
inline constexpr const char* kNotificationResourcesListChanged =
    "notifications/resources/list_changed";
inline constexpr const char* kNotificationResourcesUpdated =
    "notifications/resources/updated";
inline constexpr const char* kNotificationPromptsListChanged =
    "notifications/prompts/list_changed";
inline constexpr const char* kNotificationRootsListChanged =
    "notifications/roots/list_changed";

}  // namespace mcp::methods
