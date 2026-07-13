#if defined(MCP_LOG_ENABLED)

#include <mcp/log.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mcp/sys/threading.hpp>
#include <utility>

namespace mcp {

namespace {

using Sink = std::function<void(LogLevel, const char*, const std::string&)>;

mcp::sys::mutex g_mutex;  // serializes sink swaps and line emission
Sink g_sink;
std::atomic<int> g_level{-1};  // -1 = not yet initialized from env

LogLevel level_from_env() {
    const char* env = std::getenv("MCP_LOG");
    if (env == nullptr) {
        return LogLevel::info;
    }
    if (std::strcmp(env, "trace") == 0) return LogLevel::trace;
    if (std::strcmp(env, "debug") == 0) return LogLevel::debug;
    if (std::strcmp(env, "info") == 0) return LogLevel::info;
    if (std::strcmp(env, "warn") == 0) return LogLevel::warn;
    if (std::strcmp(env, "error") == 0) return LogLevel::error;
    if (std::strcmp(env, "off") == 0) return LogLevel::off;
    return LogLevel::info;
}

int current_level() {
    int level = g_level.load(std::memory_order_relaxed);
    if (level < 0) {
        level = static_cast<int>(level_from_env());
        g_level.store(level, std::memory_order_relaxed);
    }
    return level;
}

const char* level_name(LogLevel level) {
    switch (level) {
        case LogLevel::trace: return "trace";
        case LogLevel::debug: return "debug";
        case LogLevel::info: return "info ";
        case LogLevel::warn: return "warn ";
        case LogLevel::error: return "error";
        case LogLevel::off: break;
    }
    return "?    ";
}

}  // namespace

void set_log_sink(Sink sink) {
    std::lock_guard<mcp::sys::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

void set_log_level(LogLevel level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

namespace logdetail {

bool enabled(LogLevel level) {
    return static_cast<int>(level) >= current_level();
}

void emit(LogLevel level, const char* component, const std::string& line) {
    std::lock_guard<mcp::sys::mutex> lock(g_mutex);
    if (g_sink) {
        g_sink(level, component, line);
        return;
    }
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const int millis = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch())
            .count() %
        1000);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif
    std::fprintf(stderr, "[%02d:%02d:%02d.%03d] [%s] [%s] %s\n", tm_buf.tm_hour,
                 tm_buf.tm_min, tm_buf.tm_sec, millis, level_name(level),
                 component, line.c_str());
    std::fflush(stderr);
}

}  // namespace logdetail

}  // namespace mcp

#endif  // MCP_LOG_ENABLED
