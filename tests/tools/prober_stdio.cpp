// Integration-test server that exercises the server->client direction:
// its "probe" tool kicks off a background thread issuing roots/list, a
// sampling tool-use loop, and elicitation/create back to the client, and
// "probe_result" returns the collected report. The round-trips must not run
// inside the tool handler itself: dispatch is synchronous, so blocking the
// handler would starve the read loop.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#include <mcp/mcp.hpp>

namespace {

std::mutex g_report_mutex;
mcp::json g_report;
std::atomic<bool> g_started{false};

mcp::json probe_client(mcp::ServerSession& session) {
    mcp::json report;
    mcp::Session::RequestOptions options;
    options.timeout = std::chrono::milliseconds(5000);

    auto roots = session.send_request_sync(mcp::methods::kRootsList,
                                           std::nullopt, options);
    report["roots"] = roots ? roots.value()
                            : mcp::json{{"error", roots.error().message}};

    mcp::CreateMessageParams params;
    params.messages.push_back(
        mcp::SamplingMessage{mcp::Role::User, mcp::text_content("what is 6*7?")});
    params.max_tokens = 16;
    params.tools = mcp::json::array({mcp::json{{"name", "multiply"}}});
    auto sampled = mcp::run_sampling_tool_loop(
        session, std::move(params),
        [](const mcp::ToolUseContent& use) {
            return mcp::ToolResultContent{use.id, {mcp::text_content("42")},
                                          false};
        },
        4, std::chrono::milliseconds(5000));
    report["sampling"] = sampled ? sampled.value().to_json()
                                 : mcp::json{{"error", sampled.error().message}};

    const mcp::json schema{
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}}}}},
        {"required", mcp::json::array({"name"})}};
    auto elicited = session.send_request_sync(
        mcp::methods::kElicitationCreate,
        mcp::json{{"message", "Who is probing?"}, {"requestedSchema", schema}},
        options);
    report["elicitation"] = elicited
                                ? elicited.value()
                                : mcp::json{{"error", elicited.error().message}};
    return report;
}

}  // namespace

int main() {
    std::fprintf(stderr, "prober-stdio started\n");
    mcp::Server server("prober-stdio", "0.1.0");

    mcp::ToolSpec probe;
    probe.description = "Probe client features in the background";
    probe.handler = [&server](const mcp::json&) -> mcp::CallToolResult {
        if (!g_started.exchange(true)) {
            std::thread([&server] {
                auto* session = server.session();
                if (session == nullptr) {
                    return;
                }
                auto report = probe_client(*session);
                std::lock_guard<std::mutex> lock(g_report_mutex);
                g_report = std::move(report);
            }).detach();
        }
        return {{mcp::text_content("started")}};
    };
    server.register_tool("probe", std::move(probe));

    mcp::ToolSpec probe_result;
    probe_result.description = "Fetch the probe report (or 'pending')";
    probe_result.handler = [](const mcp::json&) -> mcp::CallToolResult {
        std::lock_guard<std::mutex> lock(g_report_mutex);
        if (g_report.is_null()) {
            return {{mcp::text_content("pending")}};
        }
        return {{mcp::text_content(g_report.dump())}};
    };
    server.register_tool("probe_result", std::move(probe_result));

    return server.run(std::make_shared<mcp::StdioTransport>());
}
