#include <gtest/gtest.h>

#define MCP_LOG_COMPONENT "test"
#include <mcp/log.hpp>

#if defined(MCP_LOG_ENABLED)

#include <mutex>
#include <string>
#include <vector>

#include <mcp/server/tool_registry.hpp>

namespace {

using namespace mcp;

struct CapturedLine {
    LogLevel level;
    std::string component;
    std::string line;
};

/// RAII sink capture; restores stderr + info level on destruction.
struct LogCapture {
    std::mutex mutex;
    std::vector<CapturedLine> lines;

    LogCapture() {
        set_log_level(LogLevel::trace);
        set_log_sink([this](LogLevel level, const char* component,
                            const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex);
            lines.push_back({level, component, line});
        });
    }
    ~LogCapture() {
        set_log_sink(nullptr);
        set_log_level(LogLevel::info);
    }

    bool contains(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& entry : lines) {
            if (entry.line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

TEST(McpLog, EmitsThroughSinkWithComponentAndLevel) {
    LogCapture capture;
    MCP_LOG(info, "hello " << 42);
    ASSERT_EQ(capture.lines.size(), 1u);
    EXPECT_EQ(capture.lines[0].line, "hello 42");
    EXPECT_EQ(capture.lines[0].component, "test");
    EXPECT_EQ(capture.lines[0].level, LogLevel::info);
}

TEST(McpLog, RuntimeLevelFilters) {
    LogCapture capture;
    set_log_level(LogLevel::warn);
    MCP_LOG(info, "hidden");
    MCP_LOG(error, "shown");
    ASSERT_EQ(capture.lines.size(), 1u);
    EXPECT_EQ(capture.lines[0].line, "shown");
    EXPECT_EQ(capture.lines[0].level, LogLevel::error);
}

TEST(McpLog, FilteredStatementsDoNotEvaluateArguments) {
    LogCapture capture;
    set_log_level(LogLevel::error);
    int evaluations = 0;
    MCP_LOG(debug, "side effect " << ++evaluations);
    EXPECT_EQ(evaluations, 0);  // below threshold: stream never built
}

TEST(McpLog, RegistryEventsAreLogged) {
    LogCapture capture;
    ToolRegistry registry;
    Tool tool;
    tool.name = "logged-tool";
    ASSERT_TRUE(registry.register_tool(
        std::move(tool), [](const json&) { return CallToolResult{}; }));
    EXPECT_TRUE(capture.contains("tool registered"));
    EXPECT_TRUE(capture.contains("logged-tool"));
}

}  // namespace

#else  // !MCP_LOG_ENABLED

namespace {

TEST(McpLog, DisabledMacroCompilesAwayAndNeverEvaluates) {
    int evaluations = 0;
    MCP_LOG(info, "never " << ++evaluations);
    EXPECT_EQ(evaluations, 0);
}

}  // namespace

#endif  // MCP_LOG_ENABLED
