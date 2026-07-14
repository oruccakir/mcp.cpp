// VxWorks system-monitor MCP server: exposes RTOS introspection — tasks,
// stacks, kernel heap, CPU usage, modules, semaphores, message queues — as
// MCP tools over the Streamable HTTP transport, so an AI agent can inspect a
// live target. Kernel data access is behind monitor_backend.hpp: VxWorks
// builds link the real backend, host builds a simulated one (every response
// carries a "backend" field naming which).
//
// Host / VxWorks RTP (has a main + argv):
//   ./vxworks_monitor_server --port 3003 [--host 0.0.0.0]
//
// VxWorks DKM (kernel module — no argv, main() must not block the shell):
//   -> ld < vxworks_monitor_server.out
//   -> mcp_monitor_start 3001      # binds 0.0.0.0:<port>, returns the port
//   -> mcp_monitor_stop            # stops and frees
// The DKM entries are stdio-free (images without the ANSI stdio component
// have no __stdioFp): status is the return value — start returns the bound
// port (>0) or -1, stop returns 0 (1 if nothing was running).
//
// TLS is out of scope; bind stays on 127.0.0.1 unless overridden.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <mcp/mcp.hpp>

#include "monitor_backend.hpp"

namespace {

std::string hex(std::uint64_t value) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)value);
    return buf;
}

mcp::CallToolResult ok(mcp::json data) {
    data["backend"] = monitor::backend_name();
    return {{mcp::text_content(data.dump(2))}};
}

mcp::CallToolResult fail(const std::string& message) {
    mcp::CallToolResult result{{mcp::text_content(message)}};
    result.is_error = true;
    return result;
}

mcp::json stack_json(const monitor::TaskSnapshot& t) {
    return {{"size", t.stack_size},
            {"used", t.stack_current},
            {"peakUsed", t.stack_high},
            {"margin", t.stack_margin}};
}

mcp::json task_json(const monitor::TaskSnapshot& t) {
    return {{"name", t.name},
            {"id", hex(t.id)},
            {"priority", t.priority},
            {"status", t.status},
            {"cpuIndex", t.cpu_index},
            {"errorStatus", t.error_status},
            {"delayTicks", t.delay_ticks},
            {"entry", hex(t.entry)},
            {"stack", stack_json(t)},
            {"excStack",
             {{"size", t.exc_stack_size},
              {"used", t.exc_stack_current},
              {"peakUsed", t.exc_stack_high},
              {"margin", t.exc_stack_margin}}}};
}

mcp::json heap_json(const monitor::KernelHeapStats& h) {
    return {{"allocatedBytes", h.bytes_alloc},
            {"allocatedBlocks", h.blocks_alloc},
            {"freeBytes", h.bytes_free},
            {"freeBlocks", h.blocks_free},
            {"peakAllocatedBytes", h.peak_bytes_alloc},
            {"largestFreeBlockBytes", h.max_free_block},
            {"totalBytes", h.bytes_alloc + h.bytes_free}};
}

// Object IDs come in as shell-style hex strings ("0x60377000"); base 0 also
// tolerates plain decimal.
bool parse_object_id(const mcp::json& args, std::uint64_t& id,
                     std::string& error) {
    if (!args.contains("id") || !args.at("id").is_string()) {
        error = "missing required string argument: id";
        return false;
    }
    const std::string s = args.at("id").get<std::string>();
    char* end = nullptr;
    id = std::strtoull(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != '\0') {
        error = "not a valid object ID: " + s;
        return false;
    }
    return true;
}

mcp::json object_schema(const char* what) {
    return {{"type", "object"},
            {"properties",
             {{"id",
               {{"type", "string"},
                {"description", std::string("Hex ID of the ") + what +
                                    ", e.g. \"0x60377000\""}}}}},
            {"required", mcp::json::array({"id"})}};
}

const mcp::json kEmptySchema = {{"type", "object"}, {"properties", mcp::json::object()}};

void configure_monitor(mcp::Server& server) {
    // -- list_tasks ----------------------------------------------------------
    mcp::ToolSpec list_tasks;
    list_tasks.description =
        "List all tasks with priority, status, CPU index and stack usage. "
        "Optional statusFilter substring-matches the status (e.g. \"PEND\").";
    list_tasks.input_schema = {
        {"type", "object"},
        {"properties",
         {{"statusFilter",
           {{"type", "string"},
            {"description",
             "Only tasks whose status contains this string (READY, PEND, "
             "DELAY, SUSPEND, ...)"}}}}}};
    list_tasks.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        std::vector<monitor::TaskSnapshot> tasks;
        std::string error;
        if (!monitor::list_tasks(tasks, error)) return fail(error);

        const std::string filter = args.value("statusFilter", std::string());
        mcp::json rows = mcp::json::array();
        for (const auto& t : tasks) {
            if (!filter.empty() &&
                t.status.find(filter) == std::string::npos) {
                continue;
            }
            rows.push_back(task_json(t));
        }
        return ok({{"taskCount", rows.size()}, {"tasks", std::move(rows)}});
    };
    server.register_tool("list_tasks", std::move(list_tasks));

    // -- task_info -----------------------------------------------------------
    mcp::ToolSpec task_info;
    task_info.description =
        "Full detail for one task (execution + exception stack, priority, "
        "status, entry point) by name, decimal ID, or hex ID.";
    task_info.input_schema = {
        {"type", "object"},
        {"properties",
         {{"task",
           {{"type", "string"},
            {"description",
             "Task name (e.g. \"tShell0\") or ID (e.g. \"0x60351ba8\")"}}}}},
        {"required", mcp::json::array({"task"})}};
    task_info.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        if (!args.contains("task") || !args.at("task").is_string()) {
            return fail("missing required string argument: task");
        }
        monitor::TaskSnapshot task;
        std::string error;
        if (!monitor::task_info(args.at("task").get<std::string>(), task,
                                error)) {
            return fail(error);
        }
        return ok({{"task", task_json(task)}});
    };
    server.register_tool("task_info", std::move(task_info));

    // -- stack_summary -------------------------------------------------------
    mcp::ToolSpec stack_summary;
    stack_summary.description =
        "Stack usage for every task, sorted by peak use percentage, with "
        "warnings for >85% peak use or margin below 512 bytes.";
    stack_summary.input_schema = kEmptySchema;
    stack_summary.handler = [](const mcp::json&) -> mcp::CallToolResult {
        std::vector<monitor::TaskSnapshot> tasks;
        std::string error;
        if (!monitor::list_tasks(tasks, error)) return fail(error);

        std::sort(tasks.begin(), tasks.end(),
                  [](const monitor::TaskSnapshot& a,
                     const monitor::TaskSnapshot& b) {
                      auto pct = [](const monitor::TaskSnapshot& t) {
                          return t.stack_size ? (double)t.stack_high /
                                                    (double)t.stack_size
                                              : 0.0;
                      };
                      return pct(a) > pct(b);
                  });

        mcp::json rows = mcp::json::array();
        int warnings = 0;
        for (const auto& t : tasks) {
            unsigned pct = t.stack_size
                               ? (unsigned)(t.stack_high * 100 / t.stack_size)
                               : 0;
            mcp::json row = {{"name", t.name},
                             {"id", hex(t.id)},
                             {"size", t.stack_size},
                             {"used", t.stack_current},
                             {"peakUsed", t.stack_high},
                             {"margin", t.stack_margin},
                             {"peakUsePercent", pct}};
            if (pct > 85) {
                row["warning"] = "HIGH_USAGE";
                ++warnings;
            } else if (t.stack_margin < 512) {
                row["warning"] = "LOW_MARGIN";
                ++warnings;
            }
            rows.push_back(std::move(row));
        }
        return ok({{"taskCount", rows.size()},
                   {"warningCount", warnings},
                   {"tasks", std::move(rows)}});
    };
    server.register_tool("stack_summary", std::move(stack_summary));

    // -- memory_info ---------------------------------------------------------
    mcp::ToolSpec memory_info;
    memory_info.description =
        "Kernel heap (system memory partition) statistics: allocated, free, "
        "peak, largest free block, fragmentation.";
    memory_info.input_schema = kEmptySchema;
    memory_info.handler = [](const mcp::json&) -> mcp::CallToolResult {
        monitor::KernelHeapStats heap;
        std::string error;
        if (!monitor::kernel_heap(heap, error)) return fail(error);
        return ok({{"kernelHeap", heap_json(heap)}});
    };
    server.register_tool("memory_info", std::move(memory_info));

    // -- cpu_usage -----------------------------------------------------------
    mcp::ToolSpec cpu_usage;
    cpu_usage.description =
        "Sample per-task CPU usage over a window (default 3s, max 10s) using "
        "a task-switch hook at system-tick resolution. Blocks this session "
        "for the whole window. unattributedTicks is mostly idle time.";
    cpu_usage.input_schema = {
        {"type", "object"},
        {"properties",
         {{"seconds",
           {{"type", "integer"},
            {"description", "Sampling window in seconds (1-10, default 3)"}}},
          {"topN",
           {{"type", "integer"},
            {"description", "Return at most this many tasks (default 10)"}}}}}};
    cpu_usage.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        int seconds = args.value("seconds", 3);
        if (seconds < 1) seconds = 1;
        if (seconds > 10) seconds = 10;
        int top_n = args.value("topN", 10);
        if (top_n < 1) top_n = 1;

        monitor::CpuUsageReport report;
        std::string error;
        if (!monitor::cpu_usage((unsigned)seconds, report, error)) {
            return fail(error);
        }

        mcp::json rows = mcp::json::array();
        for (const auto& t : report.tasks) {
            if ((int)rows.size() >= top_n) break;
            rows.push_back({{"name", t.name},
                            {"id", hex(t.id)},
                            {"ticks", t.ticks},
                            {"percent", t.percent}});
        }
        return ok({{"windowSeconds", report.window_seconds},
                   {"totalTicks", report.total_ticks},
                   {"unattributedTicks", report.unattributed_ticks},
                   {"tasks", std::move(rows)}});
    };
    server.register_tool("cpu_usage", std::move(cpu_usage));

    // -- system_info ---------------------------------------------------------
    mcp::ToolSpec system_info;
    system_info.description =
        "System overview: uptime, tick rate, task and module counts, kernel "
        "heap summary.";
    system_info.input_schema = kEmptySchema;
    system_info.handler = [](const mcp::json&) -> mcp::CallToolResult {
        monitor::SystemSnapshot sys;
        std::string error;
        if (!monitor::system_info(sys, error)) return fail(error);
        return ok({{"uptimeSeconds", sys.uptime_seconds},
                   {"uptimeTicks", sys.ticks},
                   {"tickRateHz", sys.tick_rate_hz},
                   {"taskCount", sys.task_count},
                   {"moduleCount", sys.module_count},
                   {"kernelHeap", heap_json(sys.heap)}});
    };
    server.register_tool("system_info", std::move(system_info));

    // -- list_modules --------------------------------------------------------
    mcp::ToolSpec list_modules;
    list_modules.description =
        "List loaded kernel object modules with text/data/bss segment "
        "addresses and sizes.";
    list_modules.input_schema = kEmptySchema;
    list_modules.handler = [](const mcp::json&) -> mcp::CallToolResult {
        std::vector<monitor::ModuleSnapshot> modules;
        std::string error;
        if (!monitor::list_modules(modules, error)) return fail(error);

        mcp::json rows = mcp::json::array();
        for (const auto& m : modules) {
            rows.push_back(
                {{"name", m.name},
                 {"id", hex(m.id)},
                 {"group", m.group},
                 {"segments",
                  {{"text", {{"address", hex(m.text_addr)}, {"size", m.text_size}}},
                   {"data", {{"address", hex(m.data_addr)}, {"size", m.data_size}}},
                   {"bss", {{"address", hex(m.bss_addr)}, {"size", m.bss_size}}}}}});
        }
        return ok({{"moduleCount", rows.size()}, {"modules", std::move(rows)}});
    };
    server.register_tool("list_modules", std::move(list_modules));

    // -- semaphore_info ------------------------------------------------------
    mcp::ToolSpec semaphore_info;
    semaphore_info.description =
        "Inspect a semaphore by ID: type (binary/mutex/counting), state "
        "(count / full / owner task), and how many tasks are blocked on it.";
    semaphore_info.input_schema = object_schema("semaphore");
    semaphore_info.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        std::uint64_t id = 0;
        std::string error;
        if (!parse_object_id(args, id, error)) return fail(error);

        monitor::SemSnapshot sem;
        if (!monitor::sem_info(id, sem, error)) return fail(error);

        mcp::json data = {{"id", hex(sem.id)},
                          {"type", sem.type},
                          {"typeRaw", sem.type_raw},
                          {"options", sem.options},
                          {"blockedTasks", sem.blocked_tasks}};
        if (sem.has_count) data["count"] = sem.count;
        if (sem.has_full) data["full"] = sem.full;
        if (sem.has_owner) data["owner"] = hex(sem.owner);
        return ok(std::move(data));
    };
    server.register_tool("semaphore_info", std::move(semaphore_info));

    // -- msgq_info -----------------------------------------------------------
    mcp::ToolSpec msgq_info;
    msgq_info.description =
        "Inspect a message queue by ID: queued messages, blocked tasks, "
        "send/receive timeout counters, capacity limits.";
    msgq_info.input_schema = object_schema("message queue");
    msgq_info.handler = [](const mcp::json& args) -> mcp::CallToolResult {
        std::uint64_t id = 0;
        std::string error;
        if (!parse_object_id(args, id, error)) return fail(error);

        monitor::MsgQSnapshot q;
        if (!monitor::msgq_info(id, q, error)) return fail(error);
        return ok({{"id", hex(q.id)},
                   {"messagesQueued", q.num_msgs},
                   {"tasksBlocked", q.num_tasks},
                   {"sendTimeouts", q.send_timeouts},
                   {"receiveTimeouts", q.recv_timeouts},
                   {"options", q.options},
                   {"maxMessages", q.max_msgs},
                   {"maxMessageLength", q.max_msg_length}});
    };
    server.register_tool("msgq_info", std::move(msgq_info));
}

std::unique_ptr<mcp::HttpSessionServer> make_http(
    mcp::Server& server, mcp::HttpSessionServerOptions options) {
    return std::make_unique<mcp::HttpSessionServer>(
        options,
        [&server](std::shared_ptr<mcp::Transport> transport) {
            auto session = std::make_unique<mcp::ServerSession>(
                std::move(transport), server.server_options());
            server.attach(*session);
            return session;
        },
        [&server](mcp::ServerSession& session) { server.detach(session); });
}

}  // namespace

int main(int argc, char** argv) {
    mcp::HttpSessionServerOptions options;
    options.http.port = 3003;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0) {
            options.http.port =
                static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--host") == 0) {
            // Default stays 127.0.0.1 (FR-TRAN-008). Binding 0.0.0.0 exposes
            // the server to the network: front it with a TLS-terminating
            // reverse proxy and configure allowed_origins / authorize.
            options.http.host = argv[i + 1];
        }
    }

    mcp::Server server("vxworks-monitor", "1.0.0");
    configure_monitor(server);
    auto http = make_http(server, options);

    std::string error;
    if (!http->start(error)) {
        std::fprintf(stderr, "failed to start: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "vxworks-monitor (%s backend) listening on %u\n",
                 monitor::backend_name(), http->port());
    std::fflush(stderr);

    // Serve until killed. A plain sleep loop (no std::promise/future) so this
    // links without pthreads — relevant for the VxWorks builds.
    for (;;) {
        mcp::sys::this_thread::sleep_for(std::chrono::hours(1));
    }
    return 0;
}

// --- VxWorks DKM entry points ------------------------------------------------
// The kernel shell cannot pass an argv array or block on main(), so expose
// no-argv C entries. HttpSessionServer::start() is non-blocking (its accept
// task keeps serving), so the start entry returns immediately; the server
// objects are kept alive in file-scope owners until stop.
//
// No stdio here (see the file header): status is the return value, which the
// VxWorks shell echoes as "value = N".

namespace {
std::unique_ptr<mcp::Server> g_dkm_server;
std::unique_ptr<mcp::HttpSessionServer> g_dkm_http;
}  // namespace

// Returns the bound port (>0) on success, or the already-running port if
// called twice. Negative on failure: -1 could not bind/start the listener.
extern "C" int mcp_monitor_start(int port) {
    if (g_dkm_http) {
        return static_cast<int>(g_dkm_http->port());  // already running
    }
    g_dkm_server = std::make_unique<mcp::Server>("vxworks-monitor", "1.0.0");
    configure_monitor(*g_dkm_server);

    mcp::HttpSessionServerOptions options;
    options.http.host = "0.0.0.0";  // reachable from the network
    options.http.port = static_cast<std::uint16_t>(port);
    g_dkm_http = make_http(*g_dkm_server, options);

    std::string error;
    if (!g_dkm_http->start(error)) {
        g_dkm_http.reset();
        g_dkm_server.reset();
        return -1;
    }
    return static_cast<int>(g_dkm_http->port());
}

// Returns 0 if a running server was stopped, 1 if nothing was running.
extern "C" int mcp_monitor_stop(void) {
    if (!g_dkm_http) {
        return 1;
    }
    g_dkm_http->stop();  // joins the accept task before we free the server
    g_dkm_http.reset();
    g_dkm_server.reset();
    return 0;
}
