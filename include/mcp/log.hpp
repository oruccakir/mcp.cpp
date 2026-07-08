#pragma once

// MCP_LOG: build-time diagnostic logging for SDK internals.
//
// Distinct from mcp::Logger (include/mcp/server/logging.hpp), which is the
// MCP *protocol* logging feature (notifications/message sent to the client,
// FR-SRV-019..021). MCP_LOG is for the developer running the process:
// registry contents, listen addresses, request traces.
//
// - Enabled with -DMCP_ENABLE_LOGGING=ON (defines MCP_LOG_ENABLED).
//   Disabled builds compile the whole statement away: the stream expression
//   is never evaluated and its string literals do not reach the binary.
// - Output goes to **stderr** — on the stdio transport stdout is the wire,
//   and the MCP spec designates stderr as the stdio log channel.
// - Runtime threshold via the MCP_LOG environment variable:
//   trace|debug|info|warn|error|off (default info).
//
// Usage (a .cpp may define MCP_LOG_COMPONENT before including this header):
//
//   #define MCP_LOG_COMPONENT "http"
//   #include <mcp/log.hpp>
//   MCP_LOG(info, "listening on " << host << ":" << port);

#if defined(MCP_LOG_ENABLED)

#include <functional>
#include <sstream>
#include <string>

namespace mcp {

enum class LogLevel : int { trace = 0, debug, info, warn, error, off };

/// Diverts log lines from stderr (tests, embedders). nullptr restores stderr.
void set_log_sink(std::function<void(LogLevel level, const char* component,
                                     const std::string& line)>
                      sink);

/// Overrides the MCP_LOG-env-derived runtime threshold.
void set_log_level(LogLevel level);

namespace logdetail {
bool enabled(LogLevel level);
void emit(LogLevel level, const char* component, const std::string& line);
}  // namespace logdetail

}  // namespace mcp

#ifndef MCP_LOG_COMPONENT
#define MCP_LOG_COMPONENT "mcp"
#endif

#define MCP_LOG(level_, ...)                                                 \
    do {                                                                     \
        if (::mcp::logdetail::enabled(::mcp::LogLevel::level_)) {            \
            std::ostringstream mcp_log_stream_;                              \
            mcp_log_stream_ << __VA_ARGS__;                                  \
            ::mcp::logdetail::emit(::mcp::LogLevel::level_,                  \
                                   MCP_LOG_COMPONENT,                        \
                                   mcp_log_stream_.str());                   \
        }                                                                    \
    } while (0)

#else  // !MCP_LOG_ENABLED

// Compiles to nothing; the arguments are never evaluated.
#define MCP_LOG(level_, ...) \
    do {                     \
    } while (0)

#endif  // MCP_LOG_ENABLED
