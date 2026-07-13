#pragma once

#include <atomic>
#include <functional>
#include <mcp/sys/threading.hpp>
#include <optional>
#include <string>

#include <mcp/json.hpp>

namespace mcp {

/// RFC 5424 severities in increasing order (FR-SRV-019).
enum class LoggingLevel {
    Debug = 0,
    Info,
    Notice,
    Warning,
    Error,
    Critical,
    Alert,
    Emergency,
};

const char* logging_level_to_string(LoggingLevel level);
std::optional<LoggingLevel> logging_level_from_string(const std::string& name);

/// Emits notifications/message for events at or above the client-set level
/// (FR-SRV-020/021). The Server wires `set_emitter` to the active session.
class Logger {
public:
    using Emitter = std::function<void(const json& params)>;

    void set_emitter(Emitter emitter);
    void set_level(LoggingLevel level) { level_.store(level); }
    LoggingLevel level() const { return level_.load(); }

    void log(LoggingLevel level, json data,
             std::optional<std::string> logger_name = std::nullopt);

    void debug(json data) { log(LoggingLevel::Debug, std::move(data)); }
    void info(json data) { log(LoggingLevel::Info, std::move(data)); }
    void warning(json data) { log(LoggingLevel::Warning, std::move(data)); }
    void error(json data) { log(LoggingLevel::Error, std::move(data)); }

private:
    std::atomic<LoggingLevel> level_{LoggingLevel::Info};
    mcp::sys::mutex mutex_;
    Emitter emitter_;
};

}  // namespace mcp
