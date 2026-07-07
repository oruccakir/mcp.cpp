#include <mcp/server/logging.hpp>

#include <mcp/detail/json_util.hpp>

namespace mcp {

const char* logging_level_to_string(LoggingLevel level) {
    switch (level) {
        case LoggingLevel::Debug: return "debug";
        case LoggingLevel::Info: return "info";
        case LoggingLevel::Notice: return "notice";
        case LoggingLevel::Warning: return "warning";
        case LoggingLevel::Error: return "error";
        case LoggingLevel::Critical: return "critical";
        case LoggingLevel::Alert: return "alert";
        case LoggingLevel::Emergency: return "emergency";
    }
    return "info";
}

std::optional<LoggingLevel> logging_level_from_string(const std::string& name) {
    if (name == "debug") return LoggingLevel::Debug;
    if (name == "info") return LoggingLevel::Info;
    if (name == "notice") return LoggingLevel::Notice;
    if (name == "warning") return LoggingLevel::Warning;
    if (name == "error") return LoggingLevel::Error;
    if (name == "critical") return LoggingLevel::Critical;
    if (name == "alert") return LoggingLevel::Alert;
    if (name == "emergency") return LoggingLevel::Emergency;
    return std::nullopt;
}

void Logger::set_emitter(Emitter emitter) {
    std::lock_guard<std::mutex> lock(mutex_);
    emitter_ = std::move(emitter);
}

void Logger::log(LoggingLevel level, json data,
                 std::optional<std::string> logger_name) {
    if (level < level_.load()) {
        return;  // Below the client-configured threshold (FR-SRV-021).
    }
    Emitter emitter;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        emitter = emitter_;
    }
    if (!emitter) {
        return;
    }
    json params{{"level", logging_level_to_string(level)},
                {"data", std::move(data)}};
    detail::set_optional(params, "logger", logger_name);
    emitter(params);
}

}  // namespace mcp
